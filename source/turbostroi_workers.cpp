/*
 * turbostroi_workers.cpp
 *
 *  Created on: 28 авг. 2022 г.
 *      Author: mr0maks
 */

#include "gmsv_turbostroi_win32.h"
#include "turbostroi_workers.h"

#include <lua.hpp>

#include <boost/thread.hpp>
#include <boost/chrono.hpp>
#include <boost/atomic.hpp>
#include <boost/assign/list_inserter.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/policies.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>

#include <GarrysMod/FactoryLoader.hpp>
#include <GarrysMod/InterfacePointers.hpp>
#include <eiface.h>
#include <Color.h>
#include <tier0/dbg.h>
#include <game/server/iplayerinfo.h>
#include <iserver.h>
#include <tier1/convar.h>
#include <tier1/iconvar.h>

#include <unistd.h>

double target_time = 0.0;
double rate = 100.0; //FPS

std::map<int, IServerNetworkable*> trains_pos;

// --- v2 Turbostroi Logic
void threadSimulation(thread_userdata* userdata) {
	lua_State* L = userdata->L;

	while (userdata && !userdata->finished) {
		lua_settop(L,0);

		//Simulate one step
		if (userdata->current_time < target_time) {
			userdata->current_time = target_time;
			lua_pushnumber(L, Plat_FloatTime());
			lua_setglobal(L, "CurrentTime");

			//Execute think
			lua_getglobal(L,"Think");
			lua_pushboolean(L, false);
			if (lua_pcall(L, 1, 0, 0)) {
				std::string err = lua_tostring(L, -1);
				err += "\n";
				shared_print(err.c_str());
			}
		}
		else {
			//Execute think
			lua_pushnumber(L, Plat_FloatTime());
			lua_setglobal(L, "CurrentTime");

			lua_getglobal(L, "Think");
			lua_pushboolean(L, true);
			if (lua_pcall(L, 1, 0, 0)) {
				std::string err = lua_tostring(L, -1);
				err += "\n";
				shared_print(err.c_str());
			}
		}
		usleep((useconds_t)(rate * 1000));
	}

	//Release resources
	shared_print("[!] Terminating train thread\n");
	lua_close(L);
	delete userdata;
}

void threadRailnetworkSimulation(rn_thread_userdata* userdata) {
	lua_State* L = userdata->L;

	while (userdata && !userdata->finished) {
		lua_settop(L,0);

		//Simulate one step
		if (userdata->current_time < target_time) {
			userdata->current_time = target_time;
			lua_pushnumber(L, userdata->current_time);
			lua_setglobal(L,"CurrentTime");

			lua_newtable(L);
			for (auto var: trains_pos)
			{
				lua_createtable(L, 0, 3);
				float* pos = var.second->GetPVSInfo()->m_vCenter;
				lua_pushnumber(L, pos[0]);			lua_rawseti(L, -2, 1);
				lua_pushnumber(L, pos[1]);			lua_rawseti(L, -2, 2);
				lua_pushnumber(L, pos[2]);			lua_rawseti(L, -2, 3);
				lua_rawseti(L, -2, var.first);
			}
			lua_setglobal(L, "TrainsPos");

			//Execute think
			lua_getglobal(L,"Think");
			if (lua_pcall(L,0,0,0)) {
				std::string err = lua_tostring(L, -1);
				err += "\n";
				shared_print(err.c_str());
			}
		}
		usleep((useconds_t)(rate * 1000));
	}

	//Release resources
	shared_print("[!] Terminating RailNetwork thread\n");
	lua_close(L);
	delete userdata;
}

