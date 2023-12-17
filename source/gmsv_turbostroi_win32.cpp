#include "gmsv_turbostroi_win32.h"
#include "turbostroi_workers.h"

#include <sched.h>
#include <pthread.h>

#include <lua.hpp>

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/Lua/SourceCompat.h>
using namespace GarrysMod::Lua;

#include <boost/thread.hpp>
#include <boost/chrono.hpp>
#include <boost/atomic.hpp>
#include <boost/assign/list_inserter.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/policies.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>

//SourceSDK
#include <GarrysMod/FactoryLoader.hpp>
#include <GarrysMod/InterfacePointers.hpp>
#include <eiface.h>
#include <Color.h>
#include <tier0/dbg.h>
#include <game/server/iplayerinfo.h>
#include <iserver.h>
#include <tier1/convar.h>
#include <tier1/iconvar.h>

#include <sourcehook_impl.h>
using namespace SourceHook;

//------------------------------------------------------------------------------
// SourceSDK
//------------------------------------------------------------------------------
SourceHook::Impl::CSourceHookImpl g_SourceHook;
SourceHook::ISourceHook *g_SHPtr = &g_SourceHook;
int g_PLID = 0;

static SourceSDK::FactoryLoader server_loader("server");
static SourceSDK::FactoryLoader icvar_loader("vstdlib");

IVEngineServer *engineServer = NULL;
IServerGameDLL *engineServerDLL = NULL;
IPlayerInfoManager *playerInfoManager = NULL;
CGlobalVars* globalvars = NULL;

extern std::map<int, IServerNetworkable*> trains_pos;

cpu_set_t *g_workers_cpuset = NULL;
size_t	g_workers_cpuset_size = 0;

//------------------------------------------------------------------------------
// Lua Utils
//------------------------------------------------------------------------------

static void stackDump(lua_State *L) {
	int i;
	int top = lua_gettop(L);
	for (i = 1; i <= top; i++) {  /* repeat for each level */
		int t = lua_type(L, i);
		switch (t) {

		case LUA_TSTRING:  /* strings */
			ConColorMsg(Color(255, 0, 0), "`%s'", lua_tostring(L, i));
			break;

		case LUA_TBOOLEAN:  /* booleans */
			ConColorMsg(Color(255, 0, 0), lua_toboolean(L, i) ? "true" : "false");
			break;

		case LUA_TNUMBER:  /* numbers */
			ConColorMsg(Color(255, 0, 0), "%g", lua_tonumber(L, i));
			break;

		default:  /* other values */
			ConColorMsg(Color(255, 0, 0), "%s", lua_typename(L, t));
			break;

		}
		ConColorMsg(Color(255, 0, 0), "  ");  /* put a separator */
	}
	ConColorMsg(Color(255, 0, 0), "\n");  /* end the listing */
}

//------------------------------------------------------------------------------
// Shared thread printing stuff
//------------------------------------------------------------------------------
#define BUFFER_SIZE 131072
#define QUEUE_SIZE 32768

char metrostroiSystemsList[BUFFER_SIZE] = { 0 };
char loadSystemsList[BUFFER_SIZE] = { 0 };
int SimThreadAffinityMask = 0;

boost::unordered_map<std::string, std::string> load_files_cache;

rn_thread_userdata* rn_userdata = NULL;
//------------------------------------------------------------------------------
// Turbostroi sim thread API
//------------------------------------------------------------------------------

boost::lockfree::queue <shared_message, boost::lockfree::fixed_sized<true>, boost::lockfree::capacity<64>> printMessages;

int shared_print(lua_State* L) {
	int n = lua_gettop(L);
	int i;
	char buffer[512];
	char* buf = buffer;
	buffer[0] = 0;

	lua_getglobal(L, "tostring");
	for (i = 1; i <= n; i++) {
		const char* str;
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		str = lua_tostring(L, -1);
		if (strlen(str) + strlen(buffer) < 512) {
			strcpy(buf, str);
			buf = buf + strlen(buf);
			buf[0] = '\t';
			buf = buf + 1;
			buf[0] = 0;
		}
		else if (i == 1 && buffer[0] == 0) {
			strcpy(buf, "[!] Message length limit reached!");
			buf = buf + strlen(buf);
			buf[0] = '\t';
			buf = buf + 1;
			buf[0] = 0;
		}
		lua_pop(L, 1);
	}
	buffer[strlen(buffer) - 1] = '\n';

	shared_message msg;
	char tempbuffer[512] = { 0 };
	strncat(tempbuffer, buffer, (512 - 1) - strlen(buffer));
	strcpy(msg.message, tempbuffer);
	printMessages.push(msg);
	return 0;
}

void shared_print(const char *str)
{
	shared_message msg;
	strncpy(msg.message, str, 512);
	printMessages.push(msg);
}

int thread_sendmessage_rpc(lua_State* state) {
	return 0;
}

int thread_recvmessages(lua_State* state) {
	lua_getglobal(state,"_userdata");
	thread_userdata* userdata = (thread_userdata*)lua_touserdata(state,-1);
	lua_pop(state,1);

	if (userdata) {
		size_t total = userdata->sim_to_thread.read_available();
		lua_createtable(state, total, 0);
		for (size_t i = 0; i < total; ++i) {
			userdata->sim_to_thread.consume_one([&](thread_msg tmsg) {
				lua_createtable(state, 0, 5);
				lua_pushnumber(state, tmsg.message);     lua_rawseti(state, -2, 1);
				lua_pushstring(state, tmsg.system_name); lua_rawseti(state, -2, 2);
				lua_pushstring(state, tmsg.name);        lua_rawseti(state, -2, 3);
				lua_pushnumber(state, tmsg.index);       lua_rawseti(state, -2, 4);
				lua_pushnumber(state, tmsg.value);       lua_rawseti(state, -2, 5);
				lua_rawseti(state, -2, i);
				});
		}
		return 1;
	}
	return 0;
}

int thread_rnrecvmessages(lua_State* state) {
	if (rn_userdata) {
		size_t total = rn_userdata->sim_to_thread.read_available();
		lua_createtable(state, total, 0);
		for (size_t i = 0; i < total; ++i) {
			rn_userdata->sim_to_thread.consume_one([&](rn_thread_msg tmsg) {
				lua_createtable(state, 0, 4);
				lua_pushnumber(state, tmsg.ent_id);					lua_rawseti(state, -2, 1);
				lua_pushnumber(state, tmsg.id);						lua_rawseti(state, -2, 2);
				lua_pushstring(state, tmsg.name);					lua_rawseti(state, -2, 3);
				lua_pushnumber(state, tmsg.value);					lua_rawseti(state, -2, 4);
				lua_rawseti(state, -2, i);
				});
		}
		return 1;
	}
	return 0;
}

//------------------------------------------------------------------------------
// Metrostroi Lua API
//------------------------------------------------------------------------------
void load(GarrysMod::Lua::ILuaBase* LUA, lua_State* L, const char* filename, const char* path, const char* variable = NULL, const char* defpath = NULL, bool json = false) {
	//Load up "sv_turbostroi.lua" in the new JIT environment
	const char* file_data = NULL;
	auto cache_item = load_files_cache.find(filename);
	if (cache_item != load_files_cache.end()) {
		file_data = cache_item->second.c_str();
	}
	else {
		LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->GetField(-1, "file");
		LUA->GetField(-1, "Read");
		LUA->PushString(filename);
		LUA->PushString(path);
		LUA->Call(2, 1); //file.Read(...)
		if (LUA->GetString(-1)) {
			file_data = LUA->GetString(-1);
			load_files_cache.emplace(filename, file_data);
		}
		LUA->Pop(); //returned result
		LUA->Pop(); //file
		LUA->Pop(); //GLOB
	}

	if (file_data) {
		if (!variable) {
			if (luaL_loadbuffer(L, file_data, strlen(file_data), filename) || lua_pcall(L, 0, LUA_MULTRET, 0)) {
				char buf[8192] = { 0 };
				LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
				LUA->GetField(-1, "MsgC");
				LUA->GetField(-2, "Color");
				LUA->PushNumber(255);
				LUA->PushNumber(0);
				LUA->PushNumber(127);
				LUA->Call(3, 1);
				LUA->PushString(lua_tostring(L, -1));
				LUA->Call(2, 0);
				LUA->Pop(); //GLOB
				lua_pop(L, 1);
			}
		} else {
			if (!json) {
				lua_pushstring(L, file_data);
				lua_setglobal(L, variable);
			}
			else {
				LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
				LUA->GetField(-1, "table");
				LUA->GetField(-1, "ToString");
				LUA->Remove(-2);
				LUA->GetField(-2, "table");
				LUA->GetField(-1, "Sanitise");
				LUA->Remove(-2);
				LUA->GetField(-3, "util");
				LUA->GetField(-1, "JSONToTable");
				LUA->Remove(-2);
				LUA->PushString(file_data);
				LUA->Call(1, 1);
				LUA->Call(1, 1);
				LUA->PushString(variable);
				LUA->PushBool(false);
				LUA->Call(3, 1);

				if (luaL_loadbuffer(L, LUA->GetString(-1), strlen(LUA->GetString(-1)), filename) || lua_pcall(L, 0, LUA_MULTRET, 0)) {
					char buf[8192] = { 0 };
					LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
					LUA->GetField(-1, "MsgC");
					LUA->GetField(-2, "Color");
					LUA->PushNumber(255);
					LUA->PushNumber(0);
					LUA->PushNumber(127);
					LUA->Call(3, 1);
					LUA->PushString(lua_tostring(L, -1));
					LUA->Call(2, 0);
					LUA->Pop(); //GLOB
					lua_pop(L, 1);
				}

				LUA->Pop(); //returned result
				LUA->Pop(); //GLOB
			}
		}
	} else {
		char Message[512] = { 0 };
		if (!defpath) {
			std::sprintf(Message, "Turbostroi: File not found! ('%s' in '%s' path)\n", filename, path);
		}
		else {
			std::sprintf(Message, "Turbostroi: File not found! ('%s' in '%s' path) Trying to load default file in '%s' path\n", filename, path, defpath);
		}
		LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->GetField(-1,"MsgC");
		LUA->GetField(-2,"Color");
		LUA->PushNumber(255);
		LUA->PushNumber(0);
		LUA->PushNumber(127);
		LUA->Call(3,1);
		LUA->PushString(Message);
		LUA->Call(2,0);
		LUA->Pop(); //GLOB

		if (defpath) {
			load(LUA, L, filename, defpath, variable, NULL, json);
		}
	}
}

LUA_FUNCTION( API_InitializeTrain ) 
{
	CBaseHandle* EntHandle;
	IServerNetworkable* Entity;
	//Get entity index
	EntHandle = (CBaseHandle*)LUA->GetUserType<CBaseHandle>(1, Type::Entity);
	edict_t* EntityEdict = engineServer->PEntityOfEntIndex(EntHandle->GetEntryIndex());
	Entity = EntityEdict->GetNetworkable();
	trains_pos.insert(std::pair<int, IServerNetworkable*>(EntHandle->GetEntryIndex(), Entity));

	//Get current time
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "CurTime");
	LUA->Call(0, 1);
	double curtime = LUA->GetNumber(-1);
	LUA->Pop(2); //Curtime, GLOB

	//Initialize LuaJIT for train
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	luaopen_bit(L);
	lua_pushboolean(L, 1);
	lua_setglobal(L, "TURBOSTROI");
	lua_pushcfunction(L, shared_print);
	lua_setglobal(L, "print");
	lua_pushcfunction(L, thread_recvmessages);
	lua_setglobal(L, "RecvMessages");

	//Load neccessary files
	load(LUA, L, "metrostroi/sv_turbostroi_v2.lua", "LUA");
	load(LUA, L, "metrostroi/sh_failsim.lua", "LUA");

	//Load up all the systems
	char* systems = metrostroiSystemsList;
	while (systems[0]) {
		char filename[8192] = { 0 };
		char* system_name = systems;
		char* system_path = strchr(systems, '\n') + 1;

		strncpy(filename, system_path, strchr(system_path, '\n') - system_path);
		load(LUA, L, filename, "LUA");
		systems = system_path + strlen(filename) + 1;
	}

	//Initialize all the systems reported by the train
	systems = loadSystemsList;
	while (systems[0]) {
		char* system_name = systems;
		char* system_type = strchr(systems, '\n') + 1;
		*(strchr(system_name, '\n')) = 0;
		*(strchr(system_type, '\n')) = 0;

		lua_getglobal(L, "LoadSystems");
		lua_pushstring(L, system_type);
		lua_setfield(L, -2, system_name);

		systems = system_type + strlen(system_type) + 1;
	}
	loadSystemsList[0] = 0;

	//Initialize systems
	lua_getglobal(L, "Initialize");
	if (lua_pcall(L, 0, 0, 0)) {
		lua_pushcfunction(L, shared_print);
		lua_pushvalue(L, -2);
		lua_call(L, 1, 0);
		lua_pop(L, 1);
	}

	//New userdata
	thread_userdata* userdata = new thread_userdata();
	userdata->finished = 0;
	userdata->L = L;
	userdata->current_time = curtime;
	LUA->PushUserType(userdata, 1);
	LUA->SetField(1, "_sim_userdata");
	lua_pushlightuserdata(L, userdata);
	lua_setglobal(L, "_userdata");

	//Create thread for simulation
	boost::thread thread(threadSimulation, userdata);

	pthread_setname_np(thread.native_handle(), "trstroi_trsim");

	/*
	if (SimThreadAffinityMask) {
	//	if (!SetThreadAffinityMask(thread.native_handle(), static_cast<DWORD_PTR>(SimThreadAffinityMask))) {
		if (pthread_setaffinity_np(thread.native_handle(), g_workers_cpuset_size, g_workers_cpuset) != 0)
			ConColorMsg(Color(255, 0, 0), "Turbostroi: pthread_setaffinity_np failed on on train thread! \n");
	//		ConColorMsg(Color(255,0,0), "Turbostroi: SetSTAffinityMask failed on train thread! \n");
	//	{
	}
	*/
	return 0;
}

LUA_FUNCTION( API_DeinitializeTrain ) 
{
	//RailNetwork
	CBaseHandle* EntHandle;
	//Get entity index
	EntHandle = (CBaseHandle*)LUA->GetUserType<CBaseHandle>(1, Type::Entity);
	trains_pos.erase(EntHandle->GetEntryIndex());
	//End RailNetwork

	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = LUA->GetUserType<thread_userdata>(-1, 1);
	if (userdata) userdata->finished = 1;
	LUA->Pop();

	LUA->PushNil();
	LUA->SetField(1,"_sim_userdata");

	return 0;
}

int API_InitializeRailnetwork(ILuaBase* LUA) {
	char path_track[2048] = { 0 };
	char path_signs[2048] = { 0 };
	char path_sched[2048] = { 0 };

	//Get current time
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "CurTime");
	LUA->Call(0, 1);
	double curtime = LUA->GetNumber(-1);
	LUA->Pop(); //Curtime

	if (globalvars) {
		std::sprintf(path_track, "metrostroi_data/track_%s.txt", globalvars->mapname.ToCStr());
		std::sprintf(path_signs, "metrostroi_data/signs_%s.txt", globalvars->mapname.ToCStr());
		std::sprintf(path_sched, "metrostroi_data/sched_%s.txt", globalvars->mapname.ToCStr());
	}
	else {
		LUA->GetField(-1, "game");
		LUA->GetField(-1, "GetMap");
		LUA->Call(0, 1); //file.Read(...)
		const char* mapname = LUA->GetString(-1);
		LUA->Pop(2); //Read, GLOB

		std::sprintf(path_track, "metrostroi_data/track_%s.txt", mapname);
		std::sprintf(path_signs, "metrostroi_data/signs_%s.txt", mapname);
		std::sprintf(path_sched, "metrostroi_data/sched_%s.txt", mapname);
	}

	//Initialize LuaJIT for railnetwork
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	luaopen_bit(L);
	lua_pushboolean(L, 1);
	lua_setglobal(L, "TURBOSTROI");
	lua_pushcfunction(L, shared_print);
	lua_setglobal(L, "print");
	lua_pushcfunction(L, thread_rnrecvmessages);
	lua_setglobal(L, "RnRecvMessages");

	load(LUA, L, path_track, "DATA", "TrackLoadedData", "LUA", true);
	load(LUA, L, path_signs, "DATA", "SignsLoadedData", "LUA", true);
	load(LUA, L, path_sched, "DATA", "SchedLoadedData", "LUA", true);
	load(LUA, L, "metrostroi/sv_turbostroi_railnetwork.lua", "LUA");

	//New userdata
	rn_thread_userdata* userdata = new rn_thread_userdata();
	userdata->finished = 0;
	userdata->L = L;
	userdata->current_time = curtime;
	rn_userdata = userdata;

	//Create thread for simulation
	boost::thread thread(threadRailnetworkSimulation, userdata);

	pthread_setname_np(thread.native_handle(), "trstroi_rnsim");

	/*
	if (SimThreadAffinityMask) {
	//	if (!SetThreadAffinityMask(thread.native_handle(), static_cast<DWORD_PTR>(SimThreadAffinityMask))) {
		if (pthread_setaffinity_np(thread.native_handle(), g_workers_cpuset_size, g_workers_cpuset) != 0)
			ConColorMsg(Color(255, 0, 0), "Turbostroi: pthread_setaffinity_np failed on rail network thread! \n");
	//		ConColorMsg(Color(255, 0, 0), "Turbostroi: SetSTAffinityMask failed on rail network thread! \n");
	//	{
	}
	*/
	return 0;
}

int API_DeinitializeRailnetwork(ILuaBase* LUA)
{
	if (rn_userdata) rn_userdata->finished = 1;

	return 0;
}

LUA_FUNCTION( API_LoadSystem ) 
{
	if (LUA->GetString(1) && LUA->GetString(2)) {
		strncat(loadSystemsList,LUA->GetString(1),131071);
		strncat(loadSystemsList,"\n",131071);
		strncat(loadSystemsList,LUA->GetString(2),131071);
		strncat(loadSystemsList,"\n",131071);
	}
	return 0;
}

LUA_FUNCTION( API_RegisterSystem ) 
{
	if (LUA->GetString(1) && LUA->GetString(2)) {
		char msg[8192] = { 0 };
		LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->GetField(-1,"Msg");
		std::sprintf(msg,"Metrostroi: Registering system %s [%s]\n",LUA->GetString(1),LUA->GetString(2));
		LUA->PushString(msg);
		LUA->Call(1,0);
		LUA->Pop();

	
		strncat(metrostroiSystemsList,LUA->GetString(1),131071);
		strncat(metrostroiSystemsList,"\n",131071);
		strncat(metrostroiSystemsList,LUA->GetString(2),131071);
		strncat(metrostroiSystemsList,"\n",131071);
	}
	return 0;
}

LUA_FUNCTION( API_SendMessage ) 
{
	bool successful = true;
	LUA->CheckType(2,Type::Number);
	LUA->CheckType(3,Type::String);
	LUA->CheckType(4,Type::String);
	LUA->CheckType(5,Type::Number);
	LUA->CheckType(6,Type::Number);

	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = LUA->GetUserType<thread_userdata>(-1, 1);
	LUA->Pop();

	if (userdata) {
		thread_msg tmsg;
		tmsg.message = (int)LUA->GetNumber(2);
		strncpy(tmsg.system_name,LUA->GetString(3),63);
		tmsg.system_name[63] = 0;
		strncpy(tmsg.name,LUA->GetString(4),63);
		tmsg.name[63] = 0;
		tmsg.index = LUA->GetNumber(5);
		tmsg.value = LUA->GetNumber(6);
		if (!userdata->sim_to_thread.push(tmsg)) {
			successful = false;
		}
	} else {
		successful = false;
	}
	LUA->PushBool(successful);
	return 1;
}

LUA_FUNCTION( API_RnSendMessage ) 
{
	bool successful = true;
	LUA->CheckType(2, Type::Number);
	LUA->CheckType(3, Type::Number);
	LUA->CheckType(4, Type::String);
	LUA->CheckType(6, Type::Number);

	if (rn_userdata) {
		rn_thread_msg tmsg;
		tmsg.ent_id = LUA->GetNumber(2);
		tmsg.id = LUA->GetNumber(3);
		strncpy(tmsg.name, LUA->GetString(4), 63);
		tmsg.name[63] = 0;
		tmsg.value = LUA->GetNumber(5);
		if (!rn_userdata->sim_to_thread.push(tmsg)) {
			successful = false;
		}
	}
	else {
		successful = false;
	}
	LUA->PushBool(successful);
	return 1;
}

LUA_FUNCTION( API_RecvMessages ) 
{
	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = LUA->GetUserType<thread_userdata>(-1, 1);
	LUA->Pop();

	if (userdata) {
		LUA->CreateTable();
		for (size_t i = 0; i < userdata->thread_to_sim.read_available(); ++i) {
			userdata->thread_to_sim.consume_one([&](thread_msg tmsg) {
				LUA->PushNumber(i);
				LUA->CreateTable();
				LUA->PushNumber(1);		LUA->PushNumber(tmsg.message);			LUA->RawSet(-3);
				LUA->PushNumber(2);		LUA->PushString(tmsg.system_name);		LUA->RawSet(-3);
				LUA->PushNumber(3);		LUA->PushString(tmsg.name);				LUA->RawSet(-3);
				LUA->PushNumber(4);		LUA->PushNumber(tmsg.index);			LUA->RawSet(-3);
				LUA->PushNumber(5);		LUA->PushNumber(tmsg.value);			LUA->RawSet(-3);
				LUA->RawSet(-3);
				});
		}
		return 1;
	}
	return 0;
}

LUA_FUNCTION( API_RecvMessage ) 
{
	LUA->GetField(1, "_sim_userdata");
	thread_userdata* userdata = LUA->GetUserType<thread_userdata>(-1, 1);
	LUA->Pop();

	if (userdata) {
		thread_msg tmsg;
		if (userdata->thread_to_sim.pop(tmsg)) {
			LUA->PushNumber(tmsg.message);
			LUA->PushString(tmsg.system_name);
			LUA->PushString(tmsg.name);
			LUA->PushNumber(tmsg.index);
			LUA->PushNumber(tmsg.value);
			return 5;
		}
	}
	return 0;
}

LUA_FUNCTION( API_RnRecvMessages ) 
{
	if (rn_userdata) {
		LUA->CreateTable();
		for (size_t i = 0; i < rn_userdata->thread_to_sim.read_available(); ++i) {
			rn_userdata->thread_to_sim.consume_one([&](rn_thread_msg tmsg) {
			LUA->PushNumber(i);
			LUA->CreateTable();
			LUA->PushNumber(1);		LUA->PushNumber(tmsg.ent_id);				LUA->RawSet(-3);
			LUA->PushNumber(2);		LUA->PushNumber(tmsg.id);					LUA->RawSet(-3);
			LUA->PushNumber(3);		LUA->PushString(tmsg.name);					LUA->RawSet(-3);
			LUA->PushNumber(4);		LUA->PushNumber(tmsg.value);				LUA->RawSet(-3);
			LUA->RawSet(-3);
				});
		}
		return 1;
	}
	return 0;
}

LUA_FUNCTION( API_ReadAvailable ) 
{
	LUA->GetField(1, "_sim_userdata");
	thread_userdata* userdata = LUA->GetUserType<thread_userdata>(-1, 1);
	LUA->Pop();
	LUA->PushNumber(userdata->thread_to_sim.read_available());
	return 1;
}

LUA_FUNCTION( API_SetSimulationFPS ) 
{
	LUA->CheckType(1,Type::Number);
	rate = LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION( API_SetMTAffinityMask ) 
{
	LUA->CheckType(1, Type::Number);
	int MTAffinityMask = (int)LUA->GetNumber(1);
	int cpu_avaible = sysconf(_SC_NPROCESSORS_CONF);
	cpu_set_t *cpusetp = NULL;
	size_t cpuset_size = 0;

	cpusetp = CPU_ALLOC(cpu_avaible);
	cpuset_size = CPU_ALLOC_SIZE(cpu_avaible);
	CPU_ZERO_S(cpuset_size, cpusetp);
	for(int i = 0; i < cpu_avaible; i++)
	{
		if(MTAffinityMask & (1 << i))
			CPU_SET_S(i, cpuset_size, cpusetp);
	}

	//ConColorMsg(Color(0, 255, 0), "Turbostroi: Main Thread Running on CPU%i \n", GetCurrentProcessorNumber());
	ConColorMsg(Color(0, 255, 0), "Turbostroi: Main Thread Running on CPU %i (Total available for main %i)\n", sched_getcpu(), CPU_COUNT_S(cpuset_size, cpusetp));

	/*
	//if (!SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(MTAffinityMask))) {
	if(sched_setaffinity(getpid(), cpuset_size, cpusetp) == -1) {
		//ConColorMsg(Color(255, 0, 0), "Turbostroi: SetMTAffinityMask failed! \n");
		ConColorMsg(Color(255, 0, 0), "Turbostroi: sched_setaffinity failed with error %s!\n", strerror(errno));
	}
	else {
//		ConColorMsg(Color(0, 255, 0), "Turbostroi: Changed to CPU %i\n", GetCurrentProcessorNumber());
		ConColorMsg(Color(0, 255, 0), "Turbostroi: Changed to CPU %i\n", sched_getcpu());
	}
	*/
	CPU_FREE(cpusetp);
	return 0;
}

LUA_FUNCTION( API_SetSTAffinityMask ) 
{
	LUA->CheckType(1, Type::Number);

	int SimThreadAffinityMask_new = (int)LUA->GetNumber(1);

	// workers can't apply new mask because we lose any connection to it
	//if( SimThreadAffinityMask != 0 && (SimThreadAffinityMask != SimThreadAffinityMask_new) )
	if(SimThreadAffinityMask == SimThreadAffinityMask_new)
	{
		// unimplemented
		return 0;
	}

	if( g_workers_cpuset != NULL )
		CPU_FREE(g_workers_cpuset);

	// calculate mask for workers
	int cpu_avaible = sysconf(_SC_NPROCESSORS_CONF);

	g_workers_cpuset = CPU_ALLOC(cpu_avaible);
	g_workers_cpuset_size = CPU_ALLOC_SIZE(cpu_avaible);
	CPU_ZERO_S(g_workers_cpuset_size, g_workers_cpuset);
	for(int i = 0; i < cpu_avaible; i++)
	{
		if(SimThreadAffinityMask_new & (1 << i))
			CPU_SET_S(i, g_workers_cpuset_size, g_workers_cpuset);
	}

	ConColorMsg(Color(0, 255, 0), "Turbostroi: Available CPUs for simulation: %i of %i\n", CPU_COUNT_S(g_workers_cpuset_size, g_workers_cpuset), cpu_avaible);

	SimThreadAffinityMask = SimThreadAffinityMask_new;
	return 0;
}

LUA_FUNCTION( API_StartRailNetwork ) 
{
	if (rn_userdata) {
		API_DeinitializeRailnetwork( LUA );
	}
	API_InitializeRailnetwork( LUA );
	return 0;
}

//------------------------------------------------------------------------------
// Initialization SourceSDK
//------------------------------------------------------------------------------
SH_DECL_HOOK1_void(IServerGameDLL, Think, SH_NOATTRIB, 0, bool);
static void Think_handler(bool finalTick) {
	target_time = globalvars->curtime;
	shared_message msg;
	if (printMessages.pop(msg)) {
		ConColorMsg(Color(255, 0, 255), msg.message);
	}
}

void InstallHooks() {
	ConColorMsg(Color(0, 255, 0), "Turbostroi: Installing hooks!\n");
	SH_ADD_HOOK_STATICFUNC(IServerGameDLL, Think, engineServerDLL, Think_handler, false);
}

void ClearLoadCache(const CCommand &command) {
	load_files_cache.clear();
	ConColorMsg(Color(0, 255, 0), "Turbostroi: Cache cleared!\n");
}

void InitInterfaces() {
	g_pCVar = InterfacePointers::Cvar();
	if (g_pCVar == NULL) {
		ConColorMsg(Color(255, 0, 0), "Turbostroi: Unable to load CVAR Interface! Trying to load old version!\n");
		g_pCVar = cvar = icvar_loader.GetInterface<ICvar>("VEngineCvar004");
		if (g_pCVar == NULL)
			ConColorMsg(Color(255, 0, 0), "Turbostroi: Unable to load old CVAR Interface!\n");
	}

	engineServer = InterfacePointers::VEngineServer();
	if (engineServer == NULL)
		ConColorMsg(Color(255, 0, 0), "Turbostroi: Unable to load Engine Interface!\n");

	engineServerDLL = InterfacePointers::ServerGameDLL();
	if (engineServerDLL == NULL)
		ConColorMsg(Color(255, 0, 0), "Turbostroi: Unable to load SGameDLL Interface!\n");

	playerInfoManager = server_loader.GetInterface<IPlayerInfoManager>(INTERFACEVERSION_PLAYERINFOMANAGER);
	if (playerInfoManager == NULL)
		ConColorMsg(Color(255, 0, 0), "Turbostroi: Unable to load PlayerInfoManager Interface!\n");

	if (playerInfoManager) globalvars = playerInfoManager->GetGlobalVars();
	InstallHooks();
	ConCommand* pCommand = new ConCommand("turbostroi_clear_cache", ClearLoadCache, "say something", FCVAR_NOTIFY);
	g_pCVar->RegisterConCommand(pCommand);
}

//------------------------------------------------------------------------------
// Initialization
//------------------------------------------------------------------------------
GMOD_MODULE_OPEN() {
	InitInterfaces();

	//Check whether being ran on server
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"SERVER");
	if (LUA->IsType(-1,Type::Nil)) {
		LUA->GetField(-2,"Msg");
		LUA->PushString("Metrostroi: DLL failed to initialize (gm_turbostroi.dll can only be used on server)\n");
		LUA->Call(1,0);
		return 0;
	}
	LUA->Pop(); //SERVER

	//Check for global table
	LUA->GetField(-1,"Metrostroi");
	if (LUA->IsType(-1,Type::Nil)) {
		LUA->GetField(-2,"Msg");
		LUA->PushString("Metrostroi: DLL failed to initialize (cannot be used standalone without metrostroi addon)\n");
		LUA->Call(1,0);
		return 0;
	}
	LUA->Pop(); //Metrostroi

	//Initialize API
	LUA->CreateTable();
	LUA->PushCFunction(API_InitializeTrain);
	LUA->SetField(-2,"InitializeTrain");
	LUA->PushCFunction(API_DeinitializeTrain);
	LUA->SetField(-2,"DeinitializeTrain");
	LUA->PushCFunction(API_StartRailNetwork);
	LUA->SetField(-2,"StartRailNetwork");
	LUA->PushCFunction(API_SendMessage);
	LUA->SetField(-2,"SendMessage");
	LUA->PushCFunction(API_RnSendMessage);
	LUA->SetField(-2, "RnSendMessage");

	LUA->PushCFunction(API_RecvMessages);
	LUA->SetField(-2,"RecvMessages");
	LUA->PushCFunction(API_RecvMessage);
	LUA->SetField(-2, "RecvMessage");
	LUA->PushCFunction(API_RnRecvMessages);
	LUA->SetField(-2, "RnRecvMessages");

	LUA->PushCFunction(API_LoadSystem);
	LUA->SetField(-2,"LoadSystem");
	LUA->PushCFunction(API_RegisterSystem);
	LUA->SetField(-2,"RegisterSystem");

	LUA->PushCFunction(API_ReadAvailable);
	LUA->SetField(-2, "ReadAvailable");
	LUA->PushCFunction(API_SetSimulationFPS);
	LUA->SetField(-2,"SetSimulationFPS");
	LUA->PushCFunction(API_SetMTAffinityMask);
	LUA->SetField(-2, "SetMTAffinityMask");
	LUA->PushCFunction(API_SetSTAffinityMask);
	LUA->SetField(-2, "SetSTAffinityMask");

	LUA->SetField(-2,"Turbostroi");

	//Print some information
	LUA->GetField(-1,"Msg");
	LUA->Push(-1);

	LUA->PushString("Metrostroi: DLL initialized (built " __DATE__ ")\n");
	LUA->Call(1,0);

	char msg[1024] = { 0 };
	std::sprintf(msg,"Metrostroi: Running with %i cores\n", boost::thread::hardware_concurrency());
	LUA->PushString(msg);
	LUA->Call(1,0);
	LUA->Pop();

	if (!printMessages.is_lock_free()) {
		ConColorMsg(Color(255, 0, 0), "Turbostroi: Not fully supported! \n");
	}

	return 0;
}

//------------------------------------------------------------------------------
// Deinitialization
//------------------------------------------------------------------------------
GMOD_MODULE_CLOSE() {
	ConCommand* cmd = g_pCVar->FindCommand("turbostroi_clear_cache");
	if (cmd != nullptr) {
		g_pCVar->UnregisterConCommand(cmd);
	}

	if(g_workers_cpuset != NULL)
	{
		CPU_FREE(g_workers_cpuset);
		g_workers_cpuset = NULL;
		g_workers_cpuset_size = 0;
	}

	return 0;
}
