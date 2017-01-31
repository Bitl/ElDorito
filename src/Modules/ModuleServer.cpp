#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "ModuleServer.hpp"
#include <sstream>
#include <fstream>
#include <algorithm>
#include <random>
#include <iomanip>
#include "../ElDorito.hpp"
#include "../Patches/Network.hpp"
#include "../Patches/PlayerUid.hpp"

#include "../ThirdParty/HttpRequest.hpp"
#include "../ThirdParty/rapidjson/document.h"
#include "../ThirdParty/rapidjson/writer.h"
#include "../ThirdParty/rapidjson/stringbuffer.h"
#include "../VoIP/TeamspeakClient.hpp"
#include "../Blam/BlamNetwork.hpp"
#include "../Console.hpp"
#include "../Server/VariableSynchronization.hpp"
#include "../Patches/Assassination.hpp"
#include "../Patches/Sprint.hpp"
#include "../Server/BanList.hpp"
#include "../Server/ServerChat.hpp"
#include "ModulePlayer.hpp"
#include "ModuleVoIP.hpp"
#include "../Server/VotingSystem.hpp"
#include "../Utils/Logger.hpp"

namespace
{
	bool VariableServerCountdownUpdate(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		unsigned long seconds = Modules::ModuleServer::Instance().VarServerCountdown->ValueInt;

		Pointer::Base(0x153708).Write<uint8_t>((uint8_t)seconds + 0); // player control
		Pointer::Base(0x153738).Write<uint8_t>((uint8_t)seconds + 4); // camera position
		Pointer::Base(0x1521D1).Write<uint8_t>((uint8_t)seconds + 4); // ui timer

		// Fix team notification
		if (seconds == 4)
			Pointer::Base(0x1536F0).Write<uint8_t>(2);
		else
			Pointer::Base(0x1536F0).Write<uint8_t>(3);

		return true;
	}

	bool VariableServerMaxPlayersUpdate(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		typedef char(__cdecl *NetworkSquadSessionSetMaximumPlayerCountFunc)(int count);
		auto network_squad_session_set_maximum_player_count = (NetworkSquadSessionSetMaximumPlayerCountFunc)0x439BA0;
		network_squad_session_set_maximum_player_count(Modules::ModuleServer::Instance().VarServerMaxPlayers->ValueInt);
		return true;
	}

	bool VariableServerGamePortUpdate(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		returnInfo = "Requires a game restart to apply.";
		return true;
	}

	// retrieves master server endpoints from dewrito.json
	void GetEndpoints(std::vector<std::string>& destVect, std::string endpointType)
	{
		std::ifstream in("dewrito.json", std::ios::in | std::ios::binary);
		if (in && in.is_open())
		{
			std::string contents;
			in.seekg(0, std::ios::end);
			contents.resize((unsigned int)in.tellg());
			in.seekg(0, std::ios::beg);
			in.read(&contents[0], contents.size());
			in.close();

			rapidjson::Document json;
			if (!json.Parse<0>(contents.c_str()).HasParseError() && json.IsObject())
			{
				if (json.HasMember("masterServers"))
				{
					auto& mastersArray = json["masterServers"];
					for (auto it = mastersArray.Begin(); it != mastersArray.End(); it++)
					{
						destVect.push_back((*it)[endpointType.c_str()].GetString());
					}
				}
			}
		}
	}

	DWORD WINAPI CommandServerAnnounce_Thread(LPVOID lpParam)
	{
		std::stringstream ss;
		std::vector<std::string> announceEndpoints;

		GetEndpoints(announceEndpoints, "announce");

		for (auto server : announceEndpoints)
		{
			HttpRequest req(L"ElDewrito/" + Utils::String::WidenString(Utils::Version::GetVersionString()), L"", L"");

			try
			{
				if (!req.SendRequest(Utils::String::WidenString(server + "?port=" + Modules::ModuleServer::Instance().VarServerPort->ValueString), L"GET", L"", L"", L"", NULL, 0))
				{
					ss << "Unable to connect to master server " << server << " (error: " << req.lastError << "/" << std::to_string(GetLastError()) << ")" << std::endl << std::endl;
					continue;
				}
			}
			catch(...) // TODO: find out what exception is being caused
			{
				ss << "Exception during master server announce request to " << server << std::endl << std::endl;
				continue;
			}

			// make sure the server replied with 200 OK
			std::wstring expected = L"HTTP/1.1 200 OK";
			if (req.responseHeader.length() < expected.length())
			{
				ss << "Invalid master server announce response from " << server << std::endl << std::endl;
				continue;
			}

			auto respHdr = req.responseHeader.substr(0, expected.length());
			if (respHdr.compare(expected))
			{
				ss << "Invalid master server announce response from " << server << std::endl << std::endl;
				continue;
			}

			// parse the json response
			std::string resp = std::string(req.responseBody.begin(), req.responseBody.end());
			rapidjson::Document json;
			if (json.Parse<0>(resp.c_str()).HasParseError() || !json.IsObject())
			{
				ss << "Invalid master server JSON response from " << server << std::endl << std::endl;
				continue;
			}

			if (!json.HasMember("result"))
			{
				ss << "Master server JSON response from " << server << " is missing data." << std::endl << std::endl;
				continue;
			}

			auto& result = json["result"];
			if (result["code"].GetInt() != 0)
			{
				ss << "Master server " << server << " returned error code " << result["code"].GetInt() << " (" << result["msg"].GetString() << ")" << std::endl << std::endl;
				continue;
			}
		}

		std::string errors = ss.str();
		if (errors.length() > 0)
			Utils::Logger::Instance().Log(Utils::LogTypes::Network, Utils::LogLevel::Info, "Announce: " + ss.str());

		return true;
	}

	DWORD WINAPI CommandServerUnannounce_Thread(LPVOID lpParam)
	{
		std::stringstream ss;
		std::vector<std::string> announceEndpoints;

		GetEndpoints(announceEndpoints, "announce");

		for (auto server : announceEndpoints)
		{
			HttpRequest req(L"ElDewrito/" + Utils::String::WidenString(Utils::Version::GetVersionString()), L"", L"");

			try
			{
				if (!req.SendRequest(Utils::String::WidenString(server + "?port=" + Modules::ModuleServer::Instance().VarServerPort->ValueString + "&shutdown=true"), L"GET", L"", L"", L"", NULL, 0))
				{
					ss << "Unable to connect to master server " << server << " (error: " << req.lastError << "/" << std::to_string(GetLastError()) << ")" << std::endl << std::endl;
					continue;
				}
			}
			catch (...)
			{
				ss << "Exception during master server unannounce request to " << server << std::endl << std::endl;
				continue;
			}

			// make sure the server replied with 200 OK
			std::wstring expected = L"HTTP/1.1 200 OK";
			if (req.responseHeader.length() < expected.length())
			{
				ss << "Invalid master server unannounce response from " << server << std::endl << std::endl;
				continue;
			}

			auto respHdr = req.responseHeader.substr(0, expected.length());
			if (respHdr.compare(expected))
			{
				ss << "Invalid master server unannounce response from " << server << std::endl << std::endl;
				continue;
			}

			// parse the json response
			std::string resp = std::string(req.responseBody.begin(), req.responseBody.end());
			rapidjson::Document json;
			if (json.Parse<0>(resp.c_str()).HasParseError() || !json.IsObject())
			{
				ss << "Invalid master server JSON response from " << server << std::endl << std::endl;
				continue;
			}

			if (!json.HasMember("result"))
			{
				ss << "Master server JSON response from " << server << " is missing data." << std::endl << std::endl;
				continue;
			}

			auto& result = json["result"];
			if (result["code"].GetInt() != 0)
			{
				ss << "Master server " << server << " returned error code " << result["code"].GetInt() << " (" << result["msg"].GetString() << ")" << std::endl << std::endl;
				continue;
			}
		}

		std::string errors = ss.str();
		if (errors.length() > 0)
			Utils::Logger::Instance().Log(Utils::LogTypes::Network, Utils::LogLevel::Info, "Unannounce: " + ss.str());

		return true;
	}

	bool CommandServerAnnounce(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (!Patches::Network::IsInfoSocketOpen())
			return false;

		auto thread = CreateThread(NULL, 0, CommandServerAnnounce_Thread, (LPVOID)&Arguments, 0, NULL);
		returnInfo = "Announcing to master servers...";
		return true;
	}

	bool CommandServerUnannounce(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (!Patches::Network::IsInfoSocketOpen())
			return false;

		auto thread = CreateThread(NULL, 0, CommandServerUnannounce_Thread, (LPVOID)&Arguments, 0, NULL);
		returnInfo = "Unannouncing to master servers...";
		return true;
	}

	bool VariableServerShouldAnnounceUpdate(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (!Modules::ModuleServer::Instance().VarServerShouldAnnounce->ValueInt) // if we're setting Server.ShouldAnnounce to false unannounce ourselves too
			CommandServerUnannounce(Arguments, std::string());

		return true;
	}

	bool CommandServerConnect(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		// TODO: move this into a thread so that non-responding hosts don't lag the game
		if (Arguments.size() <= 0)
		{
			returnInfo = "Invalid arguments.";
			return false;
		}

		std::string address = Arguments[0];
		std::string password = "";
		if (Arguments.size() > 1)
			password = Arguments[1];

		uint32_t rawIpaddr = 0;
		int httpPort = 11775;

		size_t portOffset = address.find(':');
		auto host = address;
		if (portOffset != std::string::npos && portOffset + 1 < address.size())
		{
			auto port = address.substr(portOffset + 1);
			httpPort = (uint16_t)std::stoi(port);
			host = address.substr(0, portOffset);
		}

		struct addrinfo* info = NULL;
		INT retval = getaddrinfo(host.c_str(), NULL, NULL, &info);
		if (retval != 0)
		{
			int error = WSAGetLastError();
			std::string ret = "Unable to lookup " + address + " (" + std::to_string(retval) + "): ";
			if (error != 0)
			{
				if (error == WSAHOST_NOT_FOUND)
					ret += "Host not found.";
				else if (error == WSANO_DATA)
					ret += "No data record found.";
				else
					ret += "Function failed with error " + std::to_string(error) + ".";
			}
			else
				ret += "Unknown error.";
			returnInfo = ret;
			return false;
		}

		struct addrinfo *ptr = NULL;
		for (ptr = info; ptr != NULL; ptr = ptr->ai_next)
		{
			if (ptr->ai_family != AF_INET)
				continue; // not ipv4

			rawIpaddr = ntohl(((sockaddr_in*)ptr->ai_addr)->sin_addr.S_un.S_addr);
			break;
		}

		freeaddrinfo(info);

		if (!rawIpaddr)
		{
			returnInfo = "Unable to lookup " + address + ": No records found.";;
			return false;
		}

		// query the server
		HttpRequest req(L"ElDewrito/" + Utils::String::WidenString(Utils::Version::GetVersionString()), L"", L"");

		std::wstring usernameStr = L"";
		std::wstring passwordStr = L"";
		if (!password.empty())
		{
			usernameStr = L"dorito";
			passwordStr = Utils::String::WidenString(password);
		}

		if (!req.SendRequest(Utils::String::WidenString("http://" + host + ":" + std::to_string(httpPort) + "/"), L"GET", usernameStr, passwordStr, L"", NULL, 0))
		{
			returnInfo = "Unable to connect to server. (error: " + std::to_string(req.lastError) + "/" + std::to_string(GetLastError()) + ")";
			return false;
		}

		// make sure the server replied with 200 OK
		std::wstring expected = L"HTTP/1.1 200 OK";
		if (req.responseHeader.length() < expected.length())
		{
			returnInfo = "Invalid server query response.";
			return false;
		}

		auto respHdr = req.responseHeader.substr(0, expected.length());
		if (respHdr.compare(expected))
		{
			returnInfo = "Invalid server query header response.";
			return false;
		}

		// parse the json response
		std::string resp = std::string(req.responseBody.begin(), req.responseBody.end());
		rapidjson::Document json;
		if (json.Parse<0>(resp.c_str()).HasParseError() || !json.IsObject())
		{
			returnInfo = "Invalid server query JSON response.";
			return false;
		}

		// make sure the json has all the members we need
		if (!json.HasMember("gameVersion") || !json["gameVersion"].IsString() ||
			!json.HasMember("eldewritoVersion") || !json["eldewritoVersion"].IsString() ||
			!json.HasMember("port") || !json["port"].IsNumber())
		{
			returnInfo = "Server query JSON response is missing data.";
			return false;
		}

		if (!json.HasMember("xnkid") || !json["xnkid"].IsString() ||
			!json.HasMember("xnaddr") || !json["xnaddr"].IsString())
		{
			returnInfo = "Incorrect password specified.";
			return false;
		}

		std::string gameVer = json["gameVersion"].GetString();
		std::string edVer = json["eldewritoVersion"].GetString();

		std::string ourGameVer((char*)Pointer(0x199C0F0));
		std::string ourEdVer = Utils::Version::GetVersionString();
		if (ourGameVer.compare(gameVer))
		{
			returnInfo = "Server is running a different game version.";
			return false;
		}

		if (ourEdVer.compare(edVer))
		{
			returnInfo = "Server is running a different ElDewrito version.";
			return false;
		}

		std::string xnkid = json["xnkid"].GetString();
		std::string xnaddr = json["xnaddr"].GetString();
		if (xnkid.length() != (0x10 * 2) || xnaddr.length() != (0x10 * 2))
		{
			returnInfo = "Server query XNet info is invalid.";
			return false;
		}
		uint16_t gamePort = (uint16_t)json["port"].GetInt();

		BYTE xnetInfo[0x20];
		Utils::String::HexStringToBytes(xnkid, xnetInfo, 0x10);
		Utils::String::HexStringToBytes(xnaddr, xnetInfo + 0x10, 0x10);

		// set up our syslink data struct
		memset(Modules::ModuleServer::Instance().SyslinkData, 0, 0x176);
		*(uint32_t*)Modules::ModuleServer::Instance().SyslinkData = 1;

		memcpy(Modules::ModuleServer::Instance().SyslinkData + 0x9E, xnetInfo, 0x20);

		*(uint32_t*)(Modules::ModuleServer::Instance().SyslinkData + 0x170) = rawIpaddr;
		*(uint16_t*)(Modules::ModuleServer::Instance().SyslinkData + 0x174) = gamePort;

		// set syslink stuff to point at our syslink data
		Pointer::Base(0x1E8E6D8).Write<uint32_t>(1);
		Pointer::Base(0x1E8E6DC).Write<uint32_t>((uint32_t)&Modules::ModuleServer::Instance().SyslinkData);

		// tell the game to start joining
		Pointer::Base(0x1E40BA8).Write<int64_t>(-1);
		Pointer::Base(0x1E40BB0).Write<uint32_t>(1);
		Pointer::Base(0x1E40BB4).Write(xnetInfo, 0x10);
		Pointer::Base(0x1E40BD4).Write(xnetInfo + 0x10, 0x10);
		Pointer::Base(0x1E40BE4).Write<uint32_t>(1);

		// start voip
		if (Modules::ModuleVoIP::Instance().VarVoIPEnabled->ValueInt == 1) 
		{
			//Make sure teamspeak is stopped before we try to start it.
			StopTeamspeakClient();
			CreateThread(0, 0, StartTeamspeakClient, 0, 0, 0);
		}

		returnInfo = "Attempting connection to " + address + "...";
		return true;
	}

	int FindPlayerByName(const std::string &name)
	{
		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished() || !session->IsHost())
			return -1;
		auto membership = &session->MembershipInfo;
		for (auto peerIdx = membership->FindFirstPeer(); peerIdx >= 0; peerIdx = membership->FindNextPeer(peerIdx))
		{
			auto playerIdx = session->MembershipInfo.GetPeerPlayer(peerIdx);
			if (playerIdx == -1)
				continue;
			auto* player = &membership->PlayerSessions[playerIdx];
			if (Utils::String::ThinString(player->Properties.DisplayName) == name)
				return playerIdx;
		}
		return -1;
	}

	std::vector<int> FindPlayersByUid(uint64_t uid)
	{
		std::vector<int> indices;
		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished() || !session->IsHost())
			return indices;
		auto membership = &session->MembershipInfo;
		for (auto peerIdx = membership->FindFirstPeer(); peerIdx >= 0; peerIdx = membership->FindNextPeer(peerIdx))
		{
			auto playerIdx = session->MembershipInfo.GetPeerPlayer(peerIdx);
			if (playerIdx == -1)
				continue;
			auto* player = &membership->PlayerSessions[playerIdx];
			if (player->Properties.Uid == uid)
				indices.push_back(playerIdx);
		}
		return indices;
	}

	void BanIp(const std::string &ip)
	{
		auto banList = Server::LoadDefaultBanList();
		banList.AddIp(ip);
		Server::SaveDefaultBanList(banList);
	}

	bool UnbanIp(const std::string &ip)
	{
		auto banList = Server::LoadDefaultBanList();
		if (!banList.RemoveIp(ip))
			return false;
		Server::SaveDefaultBanList(banList);
		return true;
	}

	enum class KickType
	{
		Kick,
		Ban
	};

	bool DoKickPlayerCommand(KickType type, const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (Arguments.size() <= 0)
		{
			returnInfo = "Invalid arguments";
			return false;
		}
		auto kickPlayerName = Utils::String::Join(Arguments);
		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished() || !session->IsHost())
		{
			returnInfo = "You must be hosting a game to use this command";
			return false;
		}
		auto playerIdx = FindPlayerByName(kickPlayerName);
		if (playerIdx < 0)
		{
			returnInfo = "Player \"" + kickPlayerName + "\" not found.";
			return false;
		}
		auto ip = session->GetPeerAddress(session->MembershipInfo.GetPlayerPeer(playerIdx)).ToString();
		if (!Blam::Network::BootPlayer(playerIdx, 4))
		{
			returnInfo = "Failed to kick player " + kickPlayerName;
			return false;
		}
		if (type == KickType::Ban)
		{
			BanIp(ip);
			returnInfo = "Added IP " + ip + " to the ban list\n";
		}
		returnInfo += "Issued kick request for player " + kickPlayerName + " (" + std::to_string(playerIdx) + ")";
		return true;
	}

	bool DoKickUidCommand(KickType type, const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (Arguments.size() != 1)
		{
			returnInfo = "Invalid arguments";
			return false;
		}
		uint64_t uid;
		if (!Patches::PlayerUid::ParseUid(Arguments[0], &uid))
		{
			returnInfo = "Invalid UID";
			return false;
		}
		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished() || !session->IsHost())
		{
			returnInfo = "You must be hosting a game to use this command";
			return false;
		}
		auto indices = FindPlayersByUid(uid);
		if (indices.size() == 0)
		{
			returnInfo = "Player with UID " + Arguments[0] + " not found.";
			return false;
		}
		auto success = false;
		for (auto playerIdx : indices)
		{
			auto ip = session->GetPeerAddress(session->MembershipInfo.GetPlayerPeer(playerIdx)).ToString();
			auto kickPlayerName = Utils::String::ThinString(session->MembershipInfo.PlayerSessions[playerIdx].Properties.DisplayName);
			if (!Blam::Network::BootPlayer(playerIdx, 4))
			{
				returnInfo += "Failed to kick player " + kickPlayerName + "\n";
				continue;
			}
			if (type == KickType::Ban)
			{
				BanIp(ip);
				returnInfo += "Added IP " + ip + " to the ban list\n";
			}
			returnInfo += "Issued kick request for player " + kickPlayerName + " (" + std::to_string(playerIdx) + ")\n";
			success = true;
		}
		return success;
	}

	bool DoKickIndexCommand(KickType type, const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (Arguments.size() != 1)
		{
			returnInfo = "Invalid arguments";
			return false;
		}
		auto index = -1;
		size_t pos = 0;
		try
		{
			index = std::stoi(Arguments[0], &pos);
		}
		catch (std::logic_error&)
		{
		}
		if (pos != Arguments[0].length() || index < 0 || index > 15)
		{
			returnInfo = "Invalid player index";
			return false;
		}
		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished() || !session->IsHost())
		{
			returnInfo = "You must be hosting a game to use this command";
			return false;
		}
		auto player = &session->MembershipInfo.PlayerSessions[index];
		auto kickPlayerName = Utils::String::ThinString(player->Properties.DisplayName);
		auto ip = session->GetPeerAddress(player->PeerIndex).ToString();
		if (!Blam::Network::BootPlayer(index, 4))
		{
			returnInfo = "Failed to kick player " + kickPlayerName;
			return false;
		}
		if (type == KickType::Ban)
		{
			BanIp(ip);
			returnInfo = "Added IP " + ip + " to the ban list\n";
		}
		returnInfo += "Issued kick request for player " + kickPlayerName + " (" + std::to_string(index) + ")";
		return true;
	}

	bool CommandServerKickPlayer(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		return DoKickPlayerCommand(KickType::Kick, Arguments, returnInfo);
	}

	bool CommandServerBanPlayer(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		return DoKickPlayerCommand(KickType::Ban, Arguments, returnInfo);
	}

	bool CommandServerKickPlayerUid(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		return DoKickUidCommand(KickType::Kick, Arguments, returnInfo);
	}

	bool CommandServerBanPlayerUid(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		return DoKickUidCommand(KickType::Ban, Arguments, returnInfo);
	}

	bool CommandServerKickPlayerIndex(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		return DoKickIndexCommand(KickType::Kick, Arguments, returnInfo);
	}

	bool CommandServerBanPlayerIndex(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		return DoKickIndexCommand(KickType::Ban, Arguments, returnInfo);
	}

	bool CommandServerBan(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (Arguments.size() != 2)
		{
			returnInfo = "Invalid arguments";
			return false;
		}
		auto banType = Arguments[0];
		if (banType == "ip")
		{
			auto ip = Arguments[1];
			BanIp(ip);
			returnInfo = "Added IP " + ip + " to the ban list";
			return true;
		}
		returnInfo = "Unsupported ban type " + banType;
		return false;
	}

	bool CommandServerUnban(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (Arguments.size() != 2)
		{
			returnInfo = "Invalid arguments";
			return false;
		}
		auto banType = Arguments[0];
		if (banType == "ip")
		{
			auto ip = Arguments[1];
			if (!UnbanIp(ip))
			{
				returnInfo = "IP " + ip + " is not banned";
				return false;
			}
			returnInfo = "Removed IP " + ip + " from the ban list";
			return true;
		}
		returnInfo = "Unsupported ban type " + banType;
		return false;
	}

	bool CommandServerListPlayers(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		std::stringstream ss;

		// TODO: check if player is in a lobby
		// TODO: find an addr where we can find this data in clients memory
		// so people could use it to find peoples UIDs and report them for cheating etc

		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished())
		{
			returnInfo = "No session found, are you in a game?";
			return false;
		}

		int playerIdx = session->MembershipInfo.FindFirstPlayer();
		while (playerIdx != -1)
		{
			auto player = session->MembershipInfo.PlayerSessions[playerIdx];
			auto name = Utils::String::ThinString(player.Properties.DisplayName);
			ss << std::dec << "[" << playerIdx << "] \"" << name << "\" (uid: " << std::hex << player.Properties.Uid;
			if (session->IsHost())
			{
				auto ip = session->GetPeerAddress(session->MembershipInfo.GetPlayerPeer(playerIdx));
				ss << ", ip: " << ip.ToString();
			}
			ss << ")" << std::endl;

			playerIdx = session->MembershipInfo.FindNextPlayer(playerIdx);
		}

		returnInfo = ss.str();
		return true;
	}

	bool CommandServerListPlayersJSON(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

		auto* session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished())
		{
			returnInfo = "No session found, are you in a game?";
			return false;
		}

		writer.StartArray();

		int playerIdx = session->MembershipInfo.FindFirstPlayer();
		while (playerIdx != -1)
		{
			auto player = session->MembershipInfo.PlayerSessions[playerIdx];
			writer.StartObject();
			writer.Key("name");
			writer.String(Utils::String::ThinString(player.Properties.DisplayName).c_str());

			writer.Key("teamIndex");
			writer.Int(player.Properties.TeamIndex);

			std::string uidStr;
			Utils::String::BytesToHexString(&player.Properties.Uid, sizeof(uint64_t), uidStr);
			writer.Key("UID");
			writer.String(uidStr.c_str());

			std::stringstream color;
			color << "#" << std::setw(6) << std::setfill('0') << std::hex << player.Properties.Customization.Colors[Blam::Players::ColorIndices::Primary];
			writer.Key("color");
			writer.String(color.str().c_str());
			writer.EndObject();

			playerIdx = session->MembershipInfo.FindNextPlayer(playerIdx);
		}

		writer.EndArray();
		returnInfo = buffer.GetString();
		return true;
	}

	bool CommandServerMode(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{

		typedef bool(__cdecl *SetGameOnlineFunc)(int a1);
		const SetGameOnlineFunc Server_Set_Mode = reinterpret_cast<SetGameOnlineFunc>(0xA7F950);
		bool retVal = Server_Set_Mode(Modules::ModuleServer::Instance().VarServerMode->ValueInt);
		if (retVal)
		{
			returnInfo = "Changed game mode to " + Modules::ModuleServer::Instance().VarServerMode->ValueString;
			return true;
		}
		returnInfo = "Hmm, weird. Are you at the main menu?";
		return true;
	}

	bool CommandServerLobbyType(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		typedef bool(__cdecl *SetLobbyType)(int a1);
		const SetLobbyType Server_Lobby_Type = reinterpret_cast<SetLobbyType>(0xA7EE70);
		bool retVal1 = Server_Lobby_Type(Modules::ModuleServer::Instance().VarServerLobbyType->ValueInt);
		if (retVal1)
		{
			returnInfo = "Changed lobby type to " + Modules::ModuleServer::Instance().VarServerLobbyType->ValueString;
			return true;
		}
		return false;
	}

	uint16_t PingId;
	bool CommandServerPing(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (Arguments.size() > 1)
		{
			returnInfo = "Invalid arguments";
			return false;
		}

		auto session = Blam::Network::GetActiveSession();
		if (!session)
		{
			returnInfo = "No session available";
			return false;
		}

		// If an IP address was passed, send to that address
		if (Arguments.size() == 1)
		{
			Blam::Network::NetworkAddress blamAddress;
			if (!Blam::Network::NetworkAddress::Parse(Arguments[0], 11774, &blamAddress))
			{
				returnInfo = "Invalid IPv4 address";
				return false;
			}
			if (!session->Gateway->Ping(blamAddress, PingId))
			{
				returnInfo = "Failed to send ping packet";
				return false;
			}
			return true;
		}
		
		// Otherwise, send to the host
		if (!session->IsEstablished())
		{
			returnInfo = "You are not in a game. Use \"ping <ip>\" instead.";
			return false;
		}
		if (session->IsHost())
		{
			returnInfo = "You can't ping yourself. Use \"ping <ip>\" to ping someone else.";
			return false;
		}
		auto membership = &session->MembershipInfo;
		auto channelIndex = membership->PeerChannels[membership->HostPeerIndex].ChannelIndex;
		if (channelIndex == -1)
		{
			returnInfo = "You are not connected to a game. Use \"ping <ip>\" instead.";
			return false;
		}
		session->Observer->Ping(0, channelIndex, PingId);
		return true;
	}

	bool CommandServerShuffleTeams(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		auto session = Blam::Network::GetActiveSession();
		if (!session || !session->IsEstablished())
		{
			returnInfo = "No session available";
			return false;
		}
		if (!session->HasTeams())
		{
			returnInfo = "Teams are not enabled";
			return false;
		}

		// Build an array of active player indices
		int players[Blam::Network::MaxPlayers];
		auto numPlayers = 0;
		auto &membership = session->MembershipInfo;
		for (auto player = membership.FindFirstPlayer(); player >= 0; player = membership.FindNextPlayer(player))
		{
			players[numPlayers] = player;
			numPlayers++;
		}

		// Shuffle it
		static std::random_device rng;
		static std::mt19937 urng(rng());
		std::shuffle(&players[0], &players[numPlayers], urng);

		// Assign the first half to blue and the second half to red
		// If there are an odd number of players, the extra player will be assigned to red
		for (auto i = 0; i < numPlayers; i++)
		{
			auto team = (i < numPlayers / 2) ? 1 : 0;
			membership.PlayerSessions[players[i]].Properties.ClientProperties.TeamIndex = team;
		}
		membership.Update();

		// Send a chat message to notify players about the shuffle
		Server::Chat::PeerBitSet peers;
		peers.set();
		Server::Chat::SendServerMessage("Teams have been shuffled.", peers);

		returnInfo = "Teams have been shuffled.";
		return true;
	}

	void PongReceived(const Blam::Network::NetworkAddress &from, uint32_t timestamp, uint16_t id, uint32_t latency)
	{
		if (id != PingId)
			return; // Only show pings sent by the ping command

		// PONG <ip> <timestamp> <latency>
		auto ipStr = from.ToString();
		auto message = "PONG " + ipStr + " " + std::to_string(timestamp) + " " + std::to_string(latency) + "ms";
		Console::WriteLine(message);
	}

	bool SprintEnabledChanged(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		auto &serverModule = Modules::ModuleServer::Instance();
		auto enabled = serverModule.VarServerSprintEnabledClient->ValueInt != 0;
		Patches::Sprint::Enable(enabled);
		return true;
	}

	bool UnlimitedSprintEnabledChanged(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		auto &serverModule = Modules::ModuleServer::Instance();
		auto unlimited = serverModule.VarServerSprintUnlimitedClient->ValueInt != 0;
		Patches::Sprint::SetUnlimited(unlimited);
		return true;
	}

	bool AssassinationDisabledChanged(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		auto &serverModule = Modules::ModuleServer::Instance();
		auto enabled = serverModule.VarServerAssassinationEnabled->ValueInt != 0;
		Patches::Assassination::Enable(enabled);
		return true;
	}
	bool VariableServerVotingEnabledUpdate(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		if (!Modules::ModuleServer::Instance().VarServerVotingEnabled->ValueInt)
		{
			Server::Voting::Disable();
			returnInfo = "Voting System Disabled";
		}
		if (Modules::ModuleServer::Instance().VarServerVotingEnabled->ValueInt)
		{
			Server::Voting::Enable();
			returnInfo = "Voting System Enabled";
		}
		return true;
	}
	bool CommandServerSubmitVote(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{

		if (Arguments.size() <= 0)
		{
			returnInfo = "You must supply a vote.";
			return false;
		}
		auto vote = -1;
		size_t pos = 0;
		try
		{
			vote = std::stoi(Arguments[0], &pos);
		}
		catch (std::logic_error&)
		{
		}
		if (pos != Arguments[0].length() || vote < 1 || vote > 5)
		{
			returnInfo = "Invalid Vote";
			return false;
		}
		Server::Voting::SendVoteToHost(vote);

		return true;
	}
	bool CommandServerCancelVote(const std::vector<std::string>& Arguments, std::string& returnInfo)
	{
		Server::Voting::CancelVoteInProgress();
		return true;
	}
}

namespace Modules
{
	ModuleServer::ModuleServer() : ModuleBase("Server")
	{
		VarServerName = AddVariableString("Name", "server_name", "The name of the server", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsReplicated), "HaloOnline Server");
		VarServerNameClient = AddVariableString("NameClient", "server_name_client", "", eCommandFlagsInternal);
		Server::VariableSynchronization::Synchronize(VarServerName, VarServerNameClient);

		VarServerMessage = AddVariableString("Message", "server_msg", "Text to display on the loading screen (limited to 512 chars)", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsReplicated), "");
		VarServerMessageClient = AddVariableString("MessageClient", "server_msg_client", "", eCommandFlagsInternal);
		Server::VariableSynchronization::Synchronize(VarServerMessage, VarServerMessageClient);

		VarServerPassword = AddVariableString("Password", "password", "The server password", eCommandFlagsArchived, "");

		VarServerCountdown = AddVariableInt("Countdown", "countdown", "The number of seconds to wait at the start of the game", eCommandFlagsArchived, 5, VariableServerCountdownUpdate);
		VarServerCountdown->ValueIntMin = 0;
		VarServerCountdown->ValueIntMax = 20;

		VarServerMaxPlayers = AddVariableInt("MaxPlayers", "maxplayers", "Maximum number of connected players", eCommandFlagsArchived, 16, VariableServerMaxPlayersUpdate);
		VarServerMaxPlayers->ValueIntMin = 1;
		VarServerMaxPlayers->ValueIntMax = 16;

		VarServerPort = AddVariableInt("Port", "server_port", "The port number the HTTP server runs on, the game uses Server.GamePort", eCommandFlagsArchived, 11775);
		VarServerPort->ValueIntMin = 1;
		VarServerPort->ValueIntMax = 0xFFFF;

		VarServerGamePort = AddVariableInt("GamePort", "server_gameport", "The port number used by Halo Online", eCommandFlagsArchived, 11774, VariableServerGamePortUpdate);
		VarServerGamePort->ValueIntMin = 1;
		VarServerGamePort->ValueIntMax = 0xFFFF;

		VarServerShouldAnnounce = AddVariableInt("ShouldAnnounce", "should_announce", "Controls whether the server will be announced to the master servers", eCommandFlagsArchived, 1, VariableServerShouldAnnounceUpdate);
		VarServerShouldAnnounce->ValueIntMin = 0;
		VarServerShouldAnnounce->ValueIntMax = 1;

		AddCommand("Connect", "connect", "Begins establishing a connection to a server", eCommandFlagsRunOnMainMenu, CommandServerConnect, { "host:port The server info to connect to", "password(string) The password for the server" });
		AddCommand("Announce", "announce", "Announces this server to the master servers", eCommandFlagsHostOnly, CommandServerAnnounce);
		AddCommand("Unannounce", "unannounce", "Notifies the master servers to remove this server", eCommandFlagsHostOnly, CommandServerUnannounce);

		AddCommand("KickPlayer", "k", "Kicks a player from the game by name (host only)", eCommandFlagsHostOnly, CommandServerKickPlayer, { "playername The name of the player to kick" });
		AddCommand("KickBanPlayer", "kb", "Kicks and IP bans a player from the game by name (host only)", eCommandFlagsHostOnly, CommandServerBanPlayer, { "playername The name of the player to ban" });

		AddCommand("KickUid", "ku", "Kicks players from the game by UID (host only)", eCommandFlagsHostOnly, CommandServerKickPlayerUid, { "uid The UID of the players to kick" });
		AddCommand("KickBanUid", "kbu", "Kicks and IP bans players from the game by UID (host only)", eCommandFlagsHostOnly, CommandServerBanPlayerUid, { "uid The UID of the players to ban" });

		AddCommand("KickIndex", "ki", "Kicks a player from the game by index (host only)", eCommandFlagsHostOnly, CommandServerKickPlayerIndex, { "index The index of the player to kick" });
		AddCommand("KickBanIndex", "kbi", "Kicks and IP bans a player from the game by index (host only)", eCommandFlagsHostOnly, CommandServerBanPlayerIndex, { "index The index of the player to ban" });

		AddCommand("AddBan", "addban", "Adds to the ban list (does NOT kick anyone)", eCommandFlagsNone, CommandServerBan, { "type The ban type (only \"ip\" is supported for now)", "val The value to add to the ban list" });
		AddCommand("Unban", "unban", "Removes from the ban list", eCommandFlagsNone, CommandServerUnban, { "type The ban type (only \"ip\" is supported for now)", "val The value to remove from the ban list" });

		AddCommand("ListPlayers", "list", "Lists players in the game", eCommandFlagsNone, CommandServerListPlayers);
		AddCommand("ListPlayersJSON", "listjson", "Returns JSON with data about the players in the game. Intended for server browser use only.", eCommandFlagsHidden, CommandServerListPlayersJSON);

		AddCommand("Ping", "ping", "Ping a server", eCommandFlagsNone, CommandServerPing, { "[ip] The IP address of the server to ping. Omit to ping the host." });

		AddCommand("ShuffleTeams", "shuffleteams", "Evenly distributes players between the red and blue teams", eCommandFlagsHostOnly, CommandServerShuffleTeams);
		AddCommand("SubmitVote", "submitvote", "Sumbits a vote", eCommandFlagsNone, CommandServerSubmitVote, { "The vote to send to the host" });

		VarServerMode = AddVariableInt("Mode", "mode", "Changes the game mode for the server. 0 = Xbox Live (Open Party); 1 = Xbox Live (Friends Only); 2 = Xbox Live (Invite Only); 3 = Online; 4 = Offline;", eCommandFlagsNone, 4, CommandServerMode);
		VarServerMode->ValueIntMin = 0;
		VarServerMode->ValueIntMax = 4;

		VarServerLobbyType = AddVariableInt("LobbyType", "lobbytype", "Changes the lobby type for the server. 0 = Campaign; 1 = Matchmaking; 2 = Multiplayer; 3 = Forge; 4 = Theater;", eCommandFlagsDontUpdateInitial, 2, CommandServerLobbyType);
		VarServerLobbyType->ValueIntMin = 0;
		VarServerLobbyType->ValueIntMax = 4;

		VarServerSprintEnabled = AddVariableInt("SprintEnabled", "sprint", "Controls whether sprint is enabled on the server", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsReplicated), 1);
		VarServerSprintEnabledClient = AddVariableInt("SprintEnabledClient", "sprint_client", "", eCommandFlagsInternal, 1, SprintEnabledChanged);
		Server::VariableSynchronization::Synchronize(VarServerSprintEnabled, VarServerSprintEnabledClient);

		VarServerSprintUnlimited = AddVariableInt("UnlimitedSprint", "unlimited_sprint", "Controls whether unlimited sprint is enabled on the server", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsReplicated), 0);
		VarServerSprintUnlimitedClient = AddVariableInt("UnlimitedSprintClient", "unlimited_sprint_client", "", eCommandFlagsInternal, 0, UnlimitedSprintEnabledChanged);
		Server::VariableSynchronization::Synchronize(VarServerSprintUnlimited, VarServerSprintUnlimitedClient);

		VarServerDualWieldEnabled = AddVariableInt("DualWieldEnabled", "dualwield", "Controls whether dual wielding is enabled on the server", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsReplicated), 1);
		VarServerDualWieldEnabledClient = AddVariableInt("DualWieldEnabledClient", "dualwield_client", "", eCommandFlagsInternal, 0);
		Server::VariableSynchronization::Synchronize(VarServerDualWieldEnabled, VarServerDualWieldEnabledClient);

		VarServerAssassinationEnabled = AddVariableInt("AssassinationEnabled", "assassination", "Controls whether assassinations are enabled on the server", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsReplicated), 1, AssassinationDisabledChanged);

		// TODO: Fine-tune these default values
		VarFloodFilterEnabled = AddVariableInt("FloodFilterEnabled", "floodfilter", "Controls whether chat flood filtering is enabled", eCommandFlagsArchived, 1);
		VarFloodMessageScoreShort = AddVariableInt("FloodMessageScoreShort", "floodscoreshort", "Sets the flood filter score for short messages", eCommandFlagsArchived, 2);
		VarFloodMessageScoreLong = AddVariableInt("FloodMessageScoreLong", "floodscorelong", "Sets the flood filter score for long messages", eCommandFlagsArchived, 5);
		VarFloodTimeoutScore = AddVariableInt("FloodTimeoutScore", "floodscoremax", "Sets the flood filter score that triggers a timeout", eCommandFlagsArchived, 10);
		VarFloodTimeoutSeconds = AddVariableInt("FloodTimeoutSeconds", "floodtimeout", "Sets the timeout period in seconds before a spammer can send messages again", eCommandFlagsArchived, 120);
		VarFloodTimeoutResetSeconds = AddVariableInt("FloodTimeoutResetSeconds", "floodtimeoutreset", "Sets the period in seconds before a spammer's next timeout is reset", eCommandFlagsArchived, 1800);

		VarChatLogEnabled = AddVariableInt("ChatLogEnabled", "chatlog", "Controls whether chat logging is enabled", eCommandFlagsArchived, 1);
		VarChatLogPath = AddVariableString("ChatLogFile", "chatlogfile", "Sets the name of the file to log chat to", eCommandFlagsArchived, "chat.log");

		VarServerVotingEnabled = AddVariableInt("VotingEnabled", "voting_enabled", "Controls whether the map voting system is enabled on this server. ", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsHostOnly), 0, VariableServerVotingEnabledUpdate);
		VarServerVotingEnabled->ValueIntMin = 0;
		VarServerVotingEnabled->ValueIntMax = 1;

		VarServerMapVotingTime = AddVariableInt("MapVotingTime", "map_voting_time", "Controls how long the vote lasts for Map Voting. ", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsHostOnly), 30);
		VarServerMapVotingTime->ValueIntMin = 0;
		VarServerMapVotingTime->ValueIntMax = 100;

		VarServerNumberOfRevotesAllowed = AddVariableInt("NumberOfRevotesAllowed", "number_of_revotes_allowed", "Controls how many revotes are allowed in the voting system ", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsHostOnly), 3);
		VarServerNumberOfRevotesAllowed->ValueIntMin = 0;
		VarServerNumberOfRevotesAllowed->ValueIntMax = 1000;

		VarServerNumberOfVotingOptions = AddVariableInt("NumberOfVotingOptions", "number_of_voting_options", "Controls how many voting options are displayed ", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsHostOnly), 3);
		VarServerNumberOfVotingOptions->ValueIntMin = 1;
		VarServerNumberOfVotingOptions->ValueIntMax = 4;

		VarServerAutoHost = AddVariableInt("AutoHost", "autohost", "Whether or not the game will automatically host a game on launch. Only works in dedi mode.", static_cast<CommandFlags>(eCommandFlagsArchived | eCommandFlagsHostOnly), 1);
		VarServerAutoHost->ValueIntMin = 0;
		VarServerAutoHost->ValueIntMax = 1;

		VarRconPassword = AddVariableString("RconPassword", "rconpassword", "Password for the remote console", eCommandFlagsArchived, "");

		AddCommand("CancelVote", "cancelvote", "Cancels the vote", eCommandFlagsHostOnly, CommandServerCancelVote);
#ifdef _DEBUG
		// Synchronization system testing
		auto syncTestServer = AddVariableInt("SyncTest", "synctest", "Sync test server", eCommandFlagsHidden);
		auto syncTestClient = AddVariableInt("SyncTestClient", "synctestclient", "Sync test client", eCommandFlagsHidden);
		Server::VariableSynchronization::Synchronize(syncTestServer, syncTestClient);
#endif

		PingId = Patches::Network::OnPong(PongReceived);
	}
}
