﻿#include "pch.h"
#include "BDS.hpp"
#include "Event.h"
#pragma region Macro
#if 0
#define WriteLog(...) print(__VA_ARGS__)
#else
#define WriteLog(...) 0
#endif
#define Py_Method(name) {#name, api_##name, 1, 0}
#define PyAPIFunction(name) static PyObject* api_##name(PyObject* , PyObject* args)
#define CheckResult(...) if (!res) return 0; return original(__VA_ARGS__)
#define CheckisPlayer(ptr) _PlayerList[(Player*)ptr]
#pragma endregion
#pragma region Global variable
static VA _cmdqueue = 0;//指令队列
static ServerNetworkHandler* _Handle = 0;
static Level* _level = 0;
static Scoreboard* _scoreboard;//计分板
static unsigned _formid = 1;//表单ID
static unordered_map<Event, vector<PyObject*>> _PyFuncs;//Py函数表
static unordered_map<Player*, bool> _PlayerList;//玩家列表
static vector<pair<string, string>> Command;//注册命令
static unordered_map<string, PyObject*> ShareData;//共享数据
static int _Damage;//伤害值
static unordered_map<PyObject*, unsigned[2]> tick;//执行队列
static queue<function<void()>> _todos;
static bool _logstate;
#pragma endregion
#pragma region Function Define
template<class T>
void inline print(const T& data) {
	cout << data << endl;
}
template<class T, class... T2>
void inline print(const T& data, T2... other) {
	cout << data;
	print(other...);
}
static Json::Value toJson(const char* s) {
	Json::Value j;
	Json::Value::Reader r(s);
	if (!r.readValue(j)) {
		cerr << "JSON" << r.getErrorString() << endl;;
	}
	return move(j);
}
static inline VA createPacket(int type) {
	VA pkt[2];
	SYMCALL("?createPacket@MinecraftPackets@@SA?AV?$shared_ptr@VPacket@@@std@@W4MinecraftPacketIds@@@Z",
		pkt, type);
	return *pkt;
}
static bool EventCall(Event e, PyObject* val) {
	bool result = true;
	int nHold = PyGILState_Check();   //检测当前线程是否拥有GIL
	PyGILState_STATE gstate = PyGILState_LOCKED;
	if (!nHold)
		gstate = PyGILState_Ensure();   //如果没有GIL，则申请获取GIL
	Py_BEGIN_ALLOW_THREADS;
	Py_BLOCK_THREADS;

	auto& List = _PyFuncs[e];
	if (!List.empty()) {
		if (e != Event::onMove)
			WriteLog("EventCall ", e);
		for (PyObject* fn : List) {
			if (PyObject_CallFunction(fn, "O", val) == Py_False)
				result = false;
			PyErr_Print();
		}
	}
	Py_CLEAR(val);

	Py_UNBLOCK_THREADS;
	Py_END_ALLOW_THREADS;
	if (!nHold)
		PyGILState_Release(gstate);    //释放当前线程的GIL
	return result;
}
static unsigned ModalFormRequestPacket(Player* p, const string& str) {
	unsigned fid = _formid++;
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(100);
		f(unsigned, pkt + 48) = fid;
		f(string, pkt + 56) = str;
		p->sendPacket(pkt);
	}
	return fid;
}
static bool TransferPacket(Player* p, const string& address, short port) {
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(85);
		f(string, pkt + 48) = address;
		f(short, pkt + 80) = port;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool TextPacket(Player* p, int mode, const string& msg) {
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(9);
		f(int, pkt + 48) = mode;
		f(string, pkt + 56) = p->getNameTag();
		f(string, pkt + 88) = msg;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool CommandRequestPacket(Player* p, const string& cmd) {
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(77);
		f(string, pkt + 48) = cmd;
		SYMCALL<VA>("?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVCommandRequestPacket@@@Z",
			_Handle, p->getNetId(), pkt);
		//p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool BossEventPacket(Player* p, string name, float per, int eventtype) {
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(74);
		f(VA, pkt + 56) = f(VA, pkt + 64) = f(VA, p->getUniqueID());
		f(int, pkt + 72) = eventtype;//0显示,1更新,2隐藏,
		f(string, pkt + 80) = name;
		f(float, pkt + 112) = per;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool setDisplayObjectivePacket(Player* p, const string& title, const string& name = "name") {
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(107);
		f(string, pkt + 48) = "sidebar";
		f(string, pkt + 80) = name;
		f(string, pkt + 112) = title;
		f(string, pkt + 144) = "dummy";
		f(char, pkt + 176) = 0;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
static bool SetScorePacket(Player* p, char type, const vector<ScorePacketInfo>& slot) {
	if (CheckisPlayer(p)) {
		VA pkt = createPacket(108);
		f(char, pkt + 48) = type;//{set,remove}
		f(vector<ScorePacketInfo>, pkt + 56) = slot;
		p->sendPacket(pkt);
		return true;
	}
	return false;
}
#pragma endregion
#pragma region Hook List
Hook(Level_tick, void, "?tick@Level@@UEAAXXZ",
	VA a1, VA a2, VA a3, VA a4) {
	original(a1, a2, a3, a4);
#if 0
	//执行todos函数
	if (!todos.empty()) {
		for (int i = 0; i < todos.size(); i++) {
			todos.front()();
			todos.pop();
		}
	}
#endif
	if (!tick.empty()) {
		int nHold = PyGILState_Check();   //检测当前线程是否拥有GIL
		PyGILState_STATE gstate = PyGILState_LOCKED;
		if (!nHold)
			gstate = PyGILState_Ensure();   //如果没有GIL，则申请获取GIL
		Py_BEGIN_ALLOW_THREADS;
		Py_BLOCK_THREADS;
		for (auto& i : tick) {
			if (!i.second[0]) {
				i.second[0] = i.second[1];
				PyObject_CallFunction(i.first, 0);
			}
			else i.second[0]--;
		}
		Py_UNBLOCK_THREADS;
		Py_END_ALLOW_THREADS;
		if (!nHold)
			PyGILState_Release(gstate);    //释放当前线程的GIL
	}
}
Hook(SPSCQueue, VA, "??0?$SPSCQueue@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@$0CAA@@@QEAA@_K@Z",
	VA _this) {
	_cmdqueue = original(_this);
	return _cmdqueue;
}
Hook(Level_Level, Level*, "??0Level@@QEAA@AEBV?$not_null@V?$NonOwnerPointer@VSoundPlayerInterface@@@Bedrock@@@gsl@@V?$unique_ptr@VLevelStorage@@U?$default_delete@VLevelStorage@@@std@@@std@@V?$unique_ptr@VLevelLooseFileStorage@@U?$default_delete@VLevelLooseFileStorage@@@std@@@4@AEAVIMinecraftEventing@@_NEAEAVScheduler@@V?$not_null@V?$NonOwnerPointer@VStructureManager@@@Bedrock@@@2@AEAVResourcePackManager@@AEAVIEntityRegistryOwner@@V?$unique_ptr@VBlockComponentFactory@@U?$default_delete@VBlockComponentFactory@@@std@@@4@V?$unique_ptr@VBlockDefinitionGroup@@U?$default_delete@VBlockDefinitionGroup@@@std@@@4@@Z",
	VA a1, VA a2, VA a3, VA a4, VA a5, VA a6, VA a7, VA a8, VA a9, VA a10, VA a11, VA a12, VA a13) {
	_level = original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
	return _level;
}
Hook(ServerNetworkHandler_ServerNetworkHandler, VA, "??0ServerNetworkHandler@@QEAA@AEAVGameCallbacks@@AEAVLevel@@AEAVNetworkHandler@@AEAVPrivateKeyManager@@AEAVServerLocator@@AEAVPacketSender@@AEAVAllowList@@PEAVPermissionsFile@@AEBVUUID@mce@@H_NAEBV?$vector@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$allocator@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@@std@@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@HAEAVMinecraftCommands@@AEAVIMinecraftApp@@AEBV?$unordered_map@UPackIdVersion@@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@U?$hash@UPackIdVersion@@@3@U?$equal_to@UPackIdVersion@@@3@V?$allocator@U?$pair@$$CBUPackIdVersion@@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@std@@@3@@std@@AEAVScheduler@@V?$NonOwnerPointer@VTextFilteringProcessor@@@Bedrock@@@Z",
	ServerNetworkHandler* _this, VA a1, VA a2, VA a3, VA a4, VA a5, VA a6, VA a7, VA a8, VA a9, VA a10, VA a11, VA a12, VA a13, VA a14, VA a15, VA a16, VA a17, VA a18, VA a19) {
	_Handle = _this;
	return original(_this, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19);
}
Hook(ChangeSettingCommand_setup, void, "?setup@ChangeSettingCommand@@SAXAEAVCommandRegistry@@@Z",//"?setup@ChangeSettingCommand@@SAXAEAVCommandRegistry@@@Z",?setup@KillCommand@@SAXAEAVCommandRegistry@@@Z
	VA _this) {
	for (auto& cmd : Command) {
		SYMCALL("?registerCommand@CommandRegistry@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEBDW4CommandPermissionLevel@@UCommandFlag@@3@Z",
			_this, cmd.first.c_str(), cmd.second.c_str(), 0, 0, 0x40);
	}
	original(_this);
}
Hook(ServerScoreboard_, Scoreboard*, "??0ServerScoreboard@@QEAA@VCommandSoftEnumRegistry@@PEAVLevelStorage@@@Z",
	VA _this, VA a2, VA a3) {
	_scoreboard = (Scoreboard*)original(_this, a2, a3);
	return _scoreboard;
}
Hook(onConsoleOutput, ostream&, "??$_Insert_string@DU?$char_traits@D@std@@_K@std@@YAAEAV?$basic_ostream@DU?$char_traits@D@std@@@0@AEAV10@QEBD_K@Z",
	ostream& _this, const char* str, VA size) {
	if (&_this == &cout) {
		bool res = EventCall(Event::onConsoleOutput, PyUnicode_FromStringAndSize(str, size));
		if (!res)return _this;
	}
	return original(_this, str, size);
}
//Hook(onConsoleOutput2, int, "printf",
//	const char* format, va_list va) {
//	bool res = EventCall(Event::onConsoleOutput, PyUnicode_FromString(va));
//	CheckResult(format, va);
//}
Hook(onConsoleInput, bool, "??$inner_enqueue@$0A@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@?$SPSCQueue@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@$0CAA@@@AEAA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
	VA _this, const string& cmd) {
	//是否开启debug模式
	static bool debug = false;
	if (cmd == "pydebug") {
		if (debug) {
			debug = false;
		}
		else {
			debug = true;
			cout << '>';
		}
		return 0;
	}
	if (debug) {
		PyRun_SimpleString(cmd.c_str());
		cout << '>';
		return 0;
	}
	bool res = EventCall(Event::onConsoleInput, PyUnicode_FromStringAndSize(cmd.c_str(), cmd.length()));
	CheckResult(_this, cmd);
}
Hook(onPlayerJoin, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVSetLocalPlayerAsInitializedPacket@@@Z",
	ServerNetworkHandler* _this, VA id,/*SetLocalPlayerAsInitializedPacket*/ VA pkt) {
	Player* p = _this->_getServerPlayer(id, pkt);
	if (p) {
		_PlayerList[p] = true;
		EventCall(Event::onPlayerJoin, PyLong_FromUnsignedLongLong((VA)p));
	}
	original(_this, id, pkt);
}
Hook(onPlayerLeft, void, "?_onPlayerLeft@ServerNetworkHandler@@AEAAXPEAVServerPlayer@@_N@Z",
	VA _this, Player* p, char v3) {
	EventCall(Event::onPlayerLeft, PyLong_FromUnsignedLongLong((VA)p));
	_PlayerList.erase(p);
	return original(_this, p, v3);
}
Hook(onUseItem, bool, "?useItemOn@GameMode@@UEAA_NAEAVItemStack@@AEBVBlockPos@@EAEBVVec3@@PEBVBlock@@@Z",
	VA _this, ItemStack* item, BlockPos* bp, char a4, VA a5, Block* b) {
	Player* p = f(Player*, _this + 8);
	short iid = item->getId();
	short iaux = item->mAuxValue;
	string iname = item->getName();
	BlockLegacy* bl = b->getBlockLegacy();
	short bid = bl->getBlockItemID();
	string bn = bl->getBlockName();
	bool res = EventCall(Event::onUseItem,
		Py_BuildValue("{s:K,s:i,s:i,s:s,s:s,s:i,s:[i,i,i]}",
			"player", p,
			"itemid", iid,
			"itemaux", iaux,
			"itemname", iname.c_str(),
			"blockname", bn.c_str(),
			"blockid", bid,
			"position", bp->x, bp->y, bp->z
		)
	);
	CheckResult(_this, item, bp, a4, a5, b);
}
Hook(onPlaceBlock, bool, "?mayPlace@BlockSource@@QEAA_NAEBVBlock@@AEBVBlockPos@@EPEAVActor@@_N@Z",
	BlockSource* _this, Block* b, BlockPos* bp, unsigned char a4, Actor* p, bool _bool) {
	bool res = true;
	if (CheckisPlayer(p)) {
		BlockLegacy* bl = b->getBlockLegacy();
		short bid = bl->getBlockItemID();
		string bn = bl->getBlockName();
		res = EventCall(Event::onPlaceBlock,
			Py_BuildValue("{s:K,s:s,s:i,s:[i,i,i]}",
				"player", p,
				"blockname", bn.c_str(),
				"blockid", bid,
				"position", bp->x, bp->y, bp->z
			)
		);
	}
	CheckResult(_this, b, bp, a4, p, _bool);
}
Hook(onDestroyBlock, bool, "?checkBlockDestroyPermissions@BlockSource@@QEAA_NAEAVActor@@AEBVBlockPos@@AEBVItemStackBase@@_N@Z",
	BlockSource* _this, Actor* p, BlockPos* bp, ItemStack* a3, bool a4) {
	BlockLegacy* bl = _this->getBlock(bp)->getBlockLegacy();
	short bid = bl->getBlockItemID();
	string bn = bl->getBlockName();
	bool res = EventCall(Event::onDestroyBlock,
		Py_BuildValue("{s:K,s:s,s:i,s:[i,i,i]}",
			"player", p,
			"blockname", bn.c_str(),
			"blockid", (int)bid,
			"position", bp->x, bp->y, bp->z
		)
	);
	CheckResult(_this, p, bp, a3, a4);
}
Hook(onOpenChest, bool, "?use@ChestBlock@@UEBA_NAEAVPlayer@@AEBVBlockPos@@E@Z",
	VA _this, Player* p, BlockPos* bp) {
	bool res = EventCall(Event::onOpenChest,
		Py_BuildValue("{s:K,s:[i,i,i]}",
			"player", p,
			"position", bp->x, bp->y, bp->z
		)
	);
	CheckResult(_this, p, bp);
}
Hook(onOpenBarrel, bool, "?use@BarrelBlock@@UEBA_NAEAVPlayer@@AEBVBlockPos@@E@Z",
	VA _this, Player* p, BlockPos* bp) {
	bool res = EventCall(Event::onOpenBarrel,
		Py_BuildValue("{s:K,s:[i,i,i]}",
			"player", p,
			"position", bp->x, bp->y, bp->z
		)
	);
	CheckResult(_this, p, bp);
}
Hook(onCloseChest, void, "?stopOpen@ChestBlockActor@@UEAAXAEAVPlayer@@@Z",
	VA _this, Player* p) {
	auto bp = (BlockPos*)(_this - 204);
	EventCall(Event::onCloseChest,
		Py_BuildValue("{s:K,s:[i,i,i]}",
			"player", p,
			"position", bp->x, bp->y, bp->z
		)
	);
	original(_this, p);
}
Hook(onCloseBarrel, void, "?stopOpen@BarrelBlockActor@@UEAAXAEAVPlayer@@@Z",
	VA _this, Player* p) {
	auto bp = (BlockPos*)(_this - 204);
	EventCall(Event::onCloseBarrel,
		Py_BuildValue("{s:K,s:[i,i,i]}",
			"player", p,
			"position", bp->x, bp->y, bp->z
		)
	);
	original(_this, p);
}
Hook(onContainerChange, void, "?containerContentChanged@LevelContainerModel@@UEAAXH@Z",
	VA a1, VA slot) {
	Actor* v3 = f(Actor*, a1 + 208);//IDA LevelContainerModel::_getContainer line 15 25
	BlockSource* bs = v3->getBlockSource();
	BlockPos* bp = (BlockPos*)(a1 + 216);
	BlockLegacy* bl = bs->getBlock(bp)->getBlockLegacy();
	short bid = bl->getBlockItemID();
	if (bid == 54 || bid == 130 || bid == 146 || bid == -203 || bid == 205 || bid == 218) {	//非箱子、桶、潜影盒的情况不作处理
		VA v5 = (*(VA(**)(VA))(*(VA*)a1 + 160))(a1);
		if (v5) {
			ItemStack* i = (ItemStack*)(*(VA(**)(VA, VA))(*(VA*)v5 + 40))(v5, slot);
			EventCall(Event::onContainerChange,
				Py_BuildValue("{s:K,s:s,s:i,s:[i,i,i],s:i,s:i,s:s,s:i,s:i}",
					"player", f(Player*, a1 + 208),
					"blockname", bl->getBlockName().c_str(),
					"blockid", bid,
					"position", bp->x, bp->y, bp->z,
					"itemid", i->getId(),
					"itemcount", i->mCount,
					"itemname", i->getName().c_str(),
					"itemaux", i->mAuxValue,
					"slot", slot
				)
			);
		}
	}
	original(a1, slot);
}
Hook(onAttack, bool, "?attack@Player@@UEAA_NAEAVActor@@@Z",
	Player* p, Actor* a) {
	bool res = EventCall(Event::onPlayerAttack,
		Py_BuildValue("{s:K,s:K}",
			"player", p,
			"actor", a
		)
	);
	CheckResult(p, a);
}
Hook(onChangeDimension, bool, "?_playerChangeDimension@Level@@AEAA_NPEAVPlayer@@AEAVChangeDimensionRequest@@@Z",
	VA _this, Player* p, VA req) {
	bool result = original(_this, p, req);
	if (result) {
		EventCall(Event::onChangeDimension, PyLong_FromUnsignedLongLong((VA)p));
	}
	return result;
}
Hook(onMobDie, void, "?die@Mob@@UEAAXAEBVActorDamageSource@@@Z",
	Mob* _this, VA dmsg) {
	char v72;
	Actor* sa = _this->getLevel()->fetchEntity(*(VA*)((*(VA(__fastcall**)(VA, char*))(*(VA*)dmsg + 64))(dmsg, &v72)));
	bool res = EventCall(Event::onMobDie,
		Py_BuildValue("{s:I,s:K,s:K}",
			"dmcase", f(unsigned, dmsg + 8),
			"actor1", _this,
			"actor2", sa//可能为0
		)
	);
	if (res) original(_this, dmsg);
}
Hook(onMobHurt, bool, "?_hurt@Mob@@MEAA_NAEBVActorDamageSource@@H_N1@Z",
	Mob* _this, VA dmsg, int a3, bool a4, bool a5) {
	_Damage = a3;//将生物受伤的值设置为可调整
	char v72;
	Actor* sa = _this->getLevel()->fetchEntity(*(VA*)((*(VA(__fastcall**)(VA, char*))(*(VA*)dmsg + 64))(dmsg, &v72)));
	bool res = EventCall(Event::onMobHurt,
		Py_BuildValue("{s:i,s:K,s:K,s:i}",
			"dmcase", f(unsigned, dmsg + 8),
			"actor1", _this,
			"actor2", sa,//可能为0
			"damage", a3
		)
	);
	CheckResult(_this, dmsg, _Damage, a4, a5);
}
Hook(onRespawn, void, "?respawn@Player@@UEAAXXZ",
	Player* p) {
	EventCall(Event::onRespawn, PyLong_FromUnsignedLongLong((VA)p));
	original(p);
}
Hook(onChat, void, "?fireEventPlayerMessage@MinecraftEventing@@AEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@000@Z",
	VA _this, string& sender, string& target, string& msg, string& style) {
	EventCall(Event::onChat, Py_BuildValue("{s:s,s:s,s:s,s:s}",
		"sender", sender.c_str(),
		"target", target.c_str(),
		"msg", msg.c_str(),
		"style", style.c_str()
	));
	original(_this, sender, target, msg, style);
}
Hook(onInputText, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVTextPacket@@@Z",
	ServerNetworkHandler* _this, VA id, /*TextPacket*/VA pkt) {
	Player* p = _this->_getServerPlayer(id, pkt);
	if (p) {
		const string& msg = f(string, pkt + 88);
		bool res = EventCall(Event::onInputText,
			Py_BuildValue("{s:K,s:s}",
				"player", p,
				"msg", msg.c_str()
			)
		);
		if (res)original(_this, id, pkt);
	}
}
Hook(onInputCommand, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVCommandRequestPacket@@@Z",
	ServerNetworkHandler* _this, VA id, /*CommandRequestPacket*/VA pkt) {
	Player* p = _this->_getServerPlayer(id, pkt);
	if (p) {
		const string& cmd = f(string, pkt + 48);
		bool res = EventCall(Event::onInputCommand,
			Py_BuildValue("{s:K,s:s}",
				"player", p,
				"cmd", cmd.c_str()
			)
		);
		if (res)original(_this, id, pkt);
	}
}
Hook(onSelectForm, void, "?handle@?$PacketHandlerDispatcherInstance@VModalFormResponsePacket@@$0A@@@UEBAXAEBVNetworkIdentifier@@AEAVNetEventCallback@@AEAV?$shared_ptr@VPacket@@@std@@@Z",
	VA _this, VA id, ServerNetworkHandler* handle,/*ModalFormResponsePacket*/VA* ppkt) {
	VA pkt = *ppkt;
	Player* p = handle->_getServerPlayer(id, pkt);
	if (p) {
		unsigned fid = f(unsigned, pkt + 48);
		string data = f(string, pkt + 56);
		if (data.back() == '\n')data.pop_back();
		EventCall(Event::onSelectForm,
			Py_BuildValue("{s:K,s:s,s:i}",
				"player", p,
				"selected", data.c_str(),
				"formid", fid
			)
		);
	}
	original(_this, id, handle, ppkt);
}
Hook(onCommandBlockUpdate, void, "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVCommandBlockUpdatePacket@@@Z",
	ServerNetworkHandler* _this, VA id, /*CommandBlockUpdatePacket*/VA pkt) {
	bool res = true;
	Player* p = _this->_getServerPlayer(id, pkt);
	if (p) {
		auto bp = f(BlockPos, pkt + 48);
		auto mode = f(unsigned short, pkt + 60);
		auto condition = f(bool, pkt + 62);
		auto redstone = f(bool, pkt + 63);
		auto cmd = f(string, pkt + 72);
		auto output = f(string, pkt + 104);
		auto rawname = f(string, pkt + 136);
		auto delay = f(int, pkt + 168);
		res = EventCall(Event::onCommandBlockUpdate,
			Py_BuildValue("{s:K,s:i,s:i,s:i,s:s,s:s,s:s,s:i,s:[i,i,i]}",
				"player", p,
				"mode", mode,
				"condition", condition,
				"redstone", redstone,
				"cmd", cmd.c_str(),
				"output", output.c_str(),
				"rawname", rawname.c_str(),
				"delay", delay,
				"position", bp.x, bp.y, bp.z
			)
		);
	}
	if (res)original(_this, id, pkt);
}
Hook(onLevelExplode, bool, "?explode@Level@@UEAAXAEAVBlockSource@@PEAVActor@@AEBVVec3@@M_N3M3@Z",
	Level* _this, BlockSource* bs, Actor* a3, Vec3 pos, float a5, bool a6, bool a7, float a8, bool a9) {
	bool res = EventCall(Event::onLevelExplode,
		Py_BuildValue("{s:K,s:[f,f,f],s:i,s:i}",
			"actor", a3,
			"position", pos.x, pos.y, pos.z,
			"dimensionid", bs->getDimensionId(),
			"power", a5
		)
	);
	CheckResult(_this, bs, a3, pos, a5, a6, a7, a8, a9);
}
Hook(onCommandBlockPerform, bool, "?_execute@CommandBlock@@AEBAXAEAVBlockSource@@AEAVCommandBlockActor@@AEBVBlockPos@@_N@Z",
	VA _this, BlockSource* a2, VA a3, BlockPos* bp, bool a5) {
	//脉冲:0,重复:1,链:2
	int mode = SYMCALL<int>("?getMode@CommandBlockActor@@QEBA?AW4CommandBlockMode@@AEAVBlockSource@@@Z", a3, a2);
	//无条件:0,有条件:1
	bool condition = SYMCALL<bool>("?getConditionalMode@CommandBlockActor@@QEBA_NAEAVBlockSource@@@Z", a3, a2);
	string cmd = f(string, a3 + 264);
	string rawname = f(string, a3 + 296);
	bool res = EventCall(Event::onCommandBlockPerform,
		Py_BuildValue("{s:i,s:b,s:s,s:s,s:[i,i,i]}",
			"mode", mode,
			"condition", condition,
			"cmd", cmd.c_str(),
			"rawname", rawname.c_str(),
			"position", bp->x, bp->y, bp->z
		)
	);
	CheckResult(_this, a2, a3, bp, a5);
}
Hook(onMove, void, "??0MovePlayerPacket@@QEAA@AEAVPlayer@@W4PositionMode@1@HH@Z",
	VA _this, Player* p, char a3, int a4, int a5) {
	EventCall(Event::onMove, PyLong_FromUnsignedLongLong((VA)p));
	original(_this, p, a3, a4, a5);
}
Hook(onSetArmor, void, "?setArmor@Player@@UEAAXW4ArmorSlot@@AEBVItemStack@@@Z",
	Player* p, unsigned slot, ItemStack* i) {
	if (!i->getId())return original(p, slot, i);
	bool res = EventCall(Event::onSetArmor,
		Py_BuildValue("{s:K,s:i,s:i,s:s,s:i,s:i}",
			"player", p,
			"itemid", i->getId(),
			"itemcount", i->mCount,
			"itemname", i->getName().c_str(),
			"itemaux", i->mAuxValue,
			"slot", slot
		)
	);
	if (res)original(p, slot, i);
}
Hook(onScoreChanged, void, "?onScoreChanged@ServerScoreboard@@UEAAXAEBUScoreboardId@@AEBVObjective@@@Z",
	const Scoreboard* _this, ScoreboardId* a2, Objective* a3) {
	/*
	原命令：
	创建计分板时：/scoreboard objectives <add|remove> <objectivename> dummy <objectivedisplayname>
	修改计分板时（此函数hook此处)：/scoreboard players <add|remove|set> <playersname> <objectivename> <playersnum>
	*/
	int scoreboardid = a2->id;
	EventCall(Event::onScoreChanged,
		Py_BuildValue("{s:i,s:i,s:s,s:s}",
			"scoreboardid", scoreboardid,
			"playersnum", a3->getPlayerScore(a2)->getCount(),
			"objectivename", a3->getScoreName().c_str(),
			"objectivedisname", a3->getScoreDisplayName().c_str()
		)
	);
	original(_this, a2, a3);
}
Hook(onFallBlockTransform, void, "?transformOnFall@FarmBlock@@UEBAXAEAVBlockSource@@AEBVBlockPos@@PEAVActor@@M@Z",
	VA _this, BlockSource* a1, BlockPos* a2, Player* p, VA a4) {
	bool res = true;
	if (CheckisPlayer(p)) {
		res = EventCall(Event::onFallBlockTransform,
			Py_BuildValue("{s:K,s:[i,i,i],s:i}",
				"player", p,
				"position", a2->x, a2->y, a2->z,
				"dimensionid", a1->getDimensionId()
			)
		);
	}
	if (res)original(_this, a1, a2, p, a4);
}
Hook(onUseRespawnAnchorBlock, bool, "?trySetSpawn@RespawnAnchorBlock@@CA_NAEAVPlayer@@AEBVBlockPos@@AEAVBlockSource@@AEAVLevel@@@Z",
	Player* p, BlockPos* a2, BlockSource* a3, void* a4) {
	bool res = true;
	if (CheckisPlayer(p)) {
		res = EventCall(Event::onUseRespawnAnchorBlock,
			Py_BuildValue("{s:K,s:[i,i,i],s:i}",
				"player", p,
				"position", a2->x, a2->y, a2->z,
				"dimensionid", a3->getDimensionId()
			)
		);
	}
	CheckResult(p, a2, a3, a4);
}
Hook(onPistonPush, bool, "?_attachedBlockWalker@PistonBlockActor@@AEAA_NAEAVBlockSource@@AEBVBlockPos@@EE@Z",
	BlockActor* _this, BlockSource* bs, BlockPos* bp, unsigned a3, unsigned a4) {
	auto blg = bs->getBlock(bp)->getBlockLegacy();
	string bn = blg->getBlockName();
	short bid = blg->getBlockItemID();
	BlockPos* bp2 = _this->getPosition();
	bool res = EventCall(Event::onPistonPush,
		Py_BuildValue("{s:s,s:i,s:[i,i,i],s:[i,i,i],s:i}",
			"blockname", bn.c_str(),
			"blockid", bid,
			"blockpos", bp->x, bp->y, bp->z,
			"pistonpos", bp2->x, bp2->y, bp2->z,
			"dimensionid", bs->getDimensionId()
		)
	);
	CheckResult(_this, bs, bp, a3, a4);
}
#pragma endregion
#pragma region API Function
//获取版本
PyAPIFunction(getVersion) {
	WriteLog(__func__);
	PyArg_ParseTuple(args, ":getVersion");
	return PyLong_FromLong(122);
}
//指令输出
PyAPIFunction(logout) {
	WriteLog(__func__);
	const char* msg;
	if (PyArg_ParseTuple(args, "s:logout", &msg)) {
		SYMCALL<ostream&>("??$_Insert_string@DU?$char_traits@D@std@@_K@std@@YAAEAV?$basic_ostream@DU?$char_traits@D@std@@@0@AEAV10@QEBD_K@Z",
			&cout, msg, strlen(msg));
		return Py_True;
	}
	return Py_False;
}
//执行指令
PyAPIFunction(runcmd) {
	WriteLog(__func__);
	const char* cmd;
	if (PyArg_ParseTuple(args, "s:runcmd", &cmd)) {
		onConsoleInput::original(_cmdqueue, cmd);
		return Py_True;
	}
	return Py_False;
}
//计时器
PyAPIFunction(setTimer) {
	WriteLog(__func__);
	unsigned time; PyObject* func;
	if (PyArg_ParseTuple(args, "OI:setTimer", &func, &time)) {
		if (!PyCallable_Check(func))
			return Py_False;
		tick[func][0] = time;
		tick[func][1] = time;
	}
	return Py_True;
}
PyAPIFunction(removeTimer) {
	WriteLog(__func__);
	PyObject* func;
	if (PyArg_ParseTuple(args, "O:removeTimer", &func, &time)) {
		tick.erase(func);
	}
	return Py_True;
}
//设置监听
PyAPIFunction(setListener) {
	const char* name; PyObject* fn;
	if (PyArg_ParseTuple(args, "sO:setListener", &name, &fn)) {
		WriteLog("setListener "s + name);
		try {
			Event e = toEvent(name);
			_PyFuncs[e].push_back(fn);
			return Py_True;
		}
		catch (const std::out_of_range&) {
			cerr << "无效的监听:" << name << endl;
		}
	}
	return Py_False;
}
//共享数据
PyAPIFunction(setShareData) {
	WriteLog(__func__);
	const char* index; PyObject* data;
	if (PyArg_ParseTuple(args, "sO:setShareData", &index, &data)) {
		ShareData[index] = data;
		return Py_True;
	}
	return Py_False;
}
PyAPIFunction(getShareData) {
	WriteLog(__func__);
	const char* index;
	if (PyArg_ParseTuple(args, "s:getShareData", &index)) {
		return ShareData[index];
	}
	return Py_False;
}
//设置指令说明
PyAPIFunction(setCommandDescription) {
	WriteLog(__func__);
	const char* cmd, * des;
	if (PyArg_ParseTuple(args, "ss:setCommandDescribe", &cmd, &des)) {
		Command.push_back({ cmd,des });
		return Py_True;
	}
	return Py_False;
}
PyAPIFunction(getPlayerList) {
	WriteLog(__func__);
	PyObject* list = PyList_New(0);
	PyArg_ParseTuple(args, ":getPlayerList");
	for (auto& p : _PlayerList) {
		if (p.second)
			PyList_Append(list, PyLong_FromUnsignedLongLong((VA)p.first));
	}
	return list;
}
//发送表单
PyAPIFunction(sendCustomForm) {
	WriteLog(__func__);
	Player* p; const char* str;
	if (PyArg_ParseTuple(args, "Ks:sendCustomForm", &p, &str)) {
		return PyLong_FromLong(ModalFormRequestPacket(p, str));
	}
	return PyLong_FromLong(0);
}
PyAPIFunction(sendSimpleForm) {
	WriteLog(__func__);
	Player* p; const char* title, * content, * buttons;
	if (PyArg_ParseTuple(args, "Ksss:sendSimpleForm", &p, &title, &content, &buttons)) {
		char str[0x400];
		sprintf(str, R"({"title":"%s","content":"%s","buttons":%s,"type":"form"})", title, content, buttons);
		return PyLong_FromLong(ModalFormRequestPacket(p, str));
	}
	return Py_False;
}
PyAPIFunction(sendModalForm) {
	WriteLog(__func__);
	Player* p; const char* title, * content, * button1, * button2;
	if (PyArg_ParseTuple(args, "Kssss:sendModalForm", &p, &title, &content, &button1, &button2)) {
		char str[0x400];
		sprintf(str, R"({"title":"%s","content":"%s","button1":"%s","button2":"%s","type":"modal"})", title, content, button1, button2);
		return PyLong_FromLong(ModalFormRequestPacket(p, str));
	}
	return Py_False;
}
//跨服传送
PyAPIFunction(transferServer) {
	WriteLog(__func__);
	Player* p; const char* address; short port;
	if (PyArg_ParseTuple(args, "Ksh:transferServer", &p, &address, &port)) {
		if (TransferPacket(p, address, port))
			return Py_True;
	}
	return Py_False;
}
//获取玩家信息
PyAPIFunction(getPlayerInfo) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerInfo", &p)) {
		if (CheckisPlayer(p)) {
			Vec3* pp = p->getPos();
			return Py_BuildValue("{s:s,s:[f,f,f],s:i,s:b,s:i,s:i,s:s,s:s}",
				"playername", p->getNameTag().c_str(),
				"XYZ", pp->x, pp->y, pp->z,
				"dimensionid", p->getDimensionId(),
				"isstand", p->isStand(),
				"health", p->getHealth(),
				"maxhealth", p->getMaxHealth(),
				"xuid", p->getXuid().c_str(),
				"uuid", p->getUuid().c_str()
			);
		}
	}
	return PyDict_New();
}
PyAPIFunction(getActorInfo) {
	WriteLog(__func__);
	Actor* a;
	if (PyArg_ParseTuple(args, "K:getActorInfo", &a)) {
		if (!a)
			return Py_False;
		Vec3* pp = a->getPos();
		return Py_BuildValue("{s:s,s:i,s:[f,f,f],s:i,s:b,s:i,s:i}",
			"entityname", a->getEntityTypeName().c_str(),
			"entityid", a->getEntityTypeId(),
			"XYZ", pp->x, pp->y, pp->z,
			"dimensionid", a->getDimensionId(),
			"isstand", a->isStand(),
			"health", a->getHealth(),
			"maxhealth", a->getMaxHealth()
		);
	}
	return Py_False;
}
PyAPIFunction(getActorInfoEx) {
	WriteLog(__func__);
	Actor* a;
	if (PyArg_ParseTuple(args, "K:getActorInfoEx", &a)) {
		if (!a)
			return Py_False;
		Tag* t = a->save();
		string info = toJson(t).toStyledString();
		t->deCompound();
		delete t;
		return PyUnicode_FromStringAndSize(info.c_str(), info.length());
	}
	return Py_False;
}
//玩家权限
PyAPIFunction(getPlayerPerm) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerPerm", &p)) {
		if (CheckisPlayer(p)) {
			return PyLong_FromLong(p->getPermission());
		}
	}
	return Py_False;
}
PyAPIFunction(setPlayerPerm) {
	WriteLog(__func__);
	Player* p; unsigned char lv;
	if (PyArg_ParseTuple(args, "Kb:setPlayerPerm", &p, &lv)) {
		if (CheckisPlayer(p)) {
			p->setPermissionLevel(lv);
			return Py_True;
		}
	}
	return Py_False;
}
//增加玩家等级
PyAPIFunction(addLevel) {
	WriteLog(__func__);
	Player* p; int lv;
	if (PyArg_ParseTuple(args, "Ki::addLevel", &p, &lv)) {
		if (CheckisPlayer(p)) {
			SYMCALL("?addLevels@Player@@UEAAXH@Z", p, lv);
			return Py_True;
		}
	}
	return Py_False;
}
//设置玩家名字
PyAPIFunction(setName) {
	WriteLog(__func__);
	Player* p; const char* name;
	if (PyArg_ParseTuple(args, "Ks:setName", &p, &name)) {
		if (CheckisPlayer(p)) {
			p->setName(name);
			return Py_True;
		}
	}
	return Py_False;
}
//玩家分数
PyAPIFunction(getPlayerScore) {
	WriteLog(__func__);
	Player* p; const char* obj;
	if (PyArg_ParseTuple(args, "Ks:getPlayerScore", &p, &obj)) {
		if (CheckisPlayer(p)) {
			Objective* testobj = _scoreboard->getObjective(obj);
			if (testobj) {
				auto id = _scoreboard->getScoreboardId(p);
				auto score = testobj->getPlayerScore(id);
				return PyLong_FromLong(score->getCount());
			}
		}
	}
	return Py_False;
}
PyAPIFunction(modifyPlayerScore) {
	WriteLog(__func__);
	Player* p; const char* obj; int count; int mode;
	if (PyArg_ParseTuple(args, "Ksii:modifyPlayerScore", &p, &obj, &count, &mode)) {
		if (CheckisPlayer(p) && mode >= 0 && mode <= 3) {
			Objective* testobj = _scoreboard->getObjective(obj);
			if (testobj) {
				//mode:{set,add,remove}
				_scoreboard->modifyPlayerScore((ScoreboardId*)_scoreboard->getScoreboardId(p), testobj, count, mode);
				return Py_True;
			}
		}
	}
	return Py_False;
}
//模拟玩家发送文本
PyAPIFunction(talkAs) {
	WriteLog(__func__);
	Player* p; const char* msg;
	if (PyArg_ParseTuple(args, "Ks:talkAs", &p, &msg)) {
		if (TextPacket(p, 1, msg))
			return Py_True;
	}
	return Py_False;
}
//模拟玩家执行指令
PyAPIFunction(runcmdAs) {
	WriteLog(__func__);
	Player* p; const char* cmd;
	if (PyArg_ParseTuple(args, "Ks:runcmdAs", &p, &cmd)) {
		if (CommandRequestPacket(p, cmd))
			return Py_True;
	}
	return Py_False;
}
//传送玩家
PyAPIFunction(teleport) {
	WriteLog(__func__);
	Player* p; float x, y, z; int did;
	if (PyArg_ParseTuple(args, "Kfffi:teleport", &p, &x, &y, &z, &did)) {
		p->teleport({ x,y,z }, did);
		return Py_True;
	}
	return Py_False;
}
//原始输出
PyAPIFunction(tellraw) {
	WriteLog(__func__);
	const char* msg;
	Player* p;
	if (PyArg_ParseTuple(args, "Ks:tellraw", &p, &msg)) {
		if (TextPacket(p, 0, msg))
			return Py_True;
	}
	return Py_False;
}
PyAPIFunction(tellrawEx) {
	WriteLog(__func__);
	const char* msg;
	Player* p; int mode;
	if (PyArg_ParseTuple(args, "Ksi:tellrawEx", &p, &msg, &mode)) {
		if (TextPacket(p, mode, msg))
			return Py_True;
	}
	return Py_False;
}
//boss栏
PyAPIFunction(setBossBar) {
	WriteLog(__func__);
	Player* p; const char* name; float per;
	if (PyArg_ParseTuple(args, "Ksf:", &p, &name, &per)) {
		if (BossEventPacket(p, name, per, 0))
			return Py_True;
	}
	return Py_False;
}
PyAPIFunction(removeBossBar) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:removeBossBar", &p)) {
		if (BossEventPacket(p, "", 0.0, 2))
			return Py_True;
	}
	return Py_False;
}
//通过玩家指针获取计分板id
PyAPIFunction(getScoreBoardId) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:getScoreBoardId", &p)) {
		if (CheckisPlayer(p)) {
			return Py_BuildValue("{s:i}",
				"scoreboardid", _scoreboard->getScoreboardId(p)
			);
		}
	}
	return Py_False;
}
PyAPIFunction(createScoreBoardId) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:createScoreBoardId", &p)) {
		if (CheckisPlayer(p)) {
			_scoreboard->createScoreBoardId(p);
			return Py_True;
		}
	}
	return Py_False;
}
//修改生物受伤的伤害值!
PyAPIFunction(setDamage) {
	WriteLog(__func__);
	int a;
	if (PyArg_ParseTuple(args, "i:setDamage", &a)) {
		_Damage = a;
		return Py_True;
	}
	return Py_False;
}
PyAPIFunction(setServerMotd) {
	WriteLog(__func__);
	const char* n;
	if (PyArg_ParseTuple(args, "s:setServerMotd", &n) && _Handle) {
		string name = n;
		SYMCALL<VA>("?allowIncomingConnections@ServerNetworkHandler@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z",
			_Handle, &name, true);
		return Py_True;
	}
	return Py_False;
}
//玩家背包相关操作
PyAPIFunction(getPlayerItems) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerItems", &p)) {
		if (CheckisPlayer(p)) {
			Json::Value j;
			for (auto& i : p->getContainer()->getSlots()) {
				j.append(toJson(i->save()));
			}
			string str = j.toStyledString();
			return PyUnicode_FromStringAndSize(str.c_str(), str.length());
		}
	}
	return Py_False;
}
PyAPIFunction(setPlayerItems) {
	WriteLog(__func__);
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:setPlayerItems", &p, &x)) {
		if (CheckisPlayer(p)) {
			Json::Value j = toJson(x);
			if (j.isArray()) {
				vector<ItemStack*> is = p->getContainer()->getSlots();
				for (unsigned i = 0; i < j.size(); i++) {
					is[i]->fromJson(j[i]);
				}
				p->sendInventroy();
				return Py_True;
			}
		}
	}
	return Py_False;
}
PyAPIFunction(getPlayerHand) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerHand", &p)) {
		if (CheckisPlayer(p)) {
			ItemStack* item = p->getSelectedItem();
			string str = toJson(item->save()).toStyledString();
			return PyUnicode_FromStringAndSize(str.c_str(), str.length());
		}
	}
	return Py_False;
}
PyAPIFunction(setPlayerHand) {
	WriteLog(__func__);
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:setPlayerHand", &p, &x)) {
		if (CheckisPlayer(p)) {
			p->getSelectedItem()->fromJson(toJson(x));
			p->sendInventroy();
			return Py_True;
		}
	}
	return Py_False;
}
PyAPIFunction(getPlayerArmor) {
	WriteLog(__func__);
	Player* p; int slot;
	if (PyArg_ParseTuple(args, "Ki:getPlayerArmor", &p, &slot)) {
		if (CheckisPlayer(p) && slot >= 0 && slot <= 4) {
			ItemStack* item = p->getArmor(slot);
			string str = toJson(item->save()).toStyledString();
			return PyUnicode_FromStringAndSize(str.c_str(), str.length());
		}
	}
	return Py_False;
}
PyAPIFunction(setPlayerArmor) {
	WriteLog(__func__);
	Player* p; int slot; const char* x;
	if (PyArg_ParseTuple(args, "Kis:setPlayerArmor", &p, &slot, &x)) {
		if (CheckisPlayer(p) && slot >= 0 && slot <= 4) {
			p->getArmor(slot)->fromJson(toJson(x));
			return Py_True;
		}
	}
	return Py_False;
}
PyAPIFunction(getPlayerItem) {
	WriteLog(__func__);
	Player* p; int slot;
	if (PyArg_ParseTuple(args, "Ki:getPlayerItem", &p, &slot)) {
		if (CheckisPlayer(p)) {
			ItemStack* item = p->getInventoryItem(slot);
			string str = toJson(item->save()).toStyledString();
			return PyUnicode_FromStringAndSize(str.c_str(), str.length());
		}
	}
	return Py_False;
}
PyAPIFunction(setPlayerItem) {
	WriteLog(__func__);
	Player* p; int slot; const char* x;
	if (PyArg_ParseTuple(args, "Kis:setPlayerItem", &p, &slot, &x)) {
		if (CheckisPlayer(p) && slot >= 0 && slot <= 36) {
			p->getInventoryItem(slot)->fromJson(toJson(x));
			return Py_True;
		}
	}
	return Py_False;
}
PyAPIFunction(getPlayerEnderChests) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:getPlayerEnderChests", &p)) {
		if (CheckisPlayer(p)) {
			Json::Value j;
			vector<ItemStack*> is = p->getEnderChestContainer()->getSlots();
			for (auto i : is) {
				j.append(toJson(i->save()));
			}
			string str = j.toStyledString();
			return PyUnicode_FromStringAndSize(str.c_str(), str.length());
		}
	}
	return Py_False;
}
PyAPIFunction(setPlayerEnderChests) {
	WriteLog(__func__);
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:setPlayerEnderChests", &p, &x)) {
		if (CheckisPlayer(p)) {
			Json::Value j = toJson(x);
			if (j.isArray()) {
				vector<ItemStack*> is = p->getEnderChestContainer()->getSlots();
				for (unsigned i = 0; i < j.size(); i++) {
					is[i]->fromJson(j[i]);
				}
				p->sendInventroy();
				return Py_True;
			}
		}
	}
	return Py_False;
}
//增加/移除物品
PyAPIFunction(addItemEx) {
	WriteLog(__func__);
	Player* p;
	const char* x;
	if (PyArg_ParseTuple(args, "Ks:addItemEx", &p, &x)) {
		if (CheckisPlayer(p)) {
			ItemStack i;
			i.fromJson(toJson(x));
			p->addItem(&i);
			p->sendInventroy();
			return Py_True;
		}
	}
	return Py_False;
}
PyAPIFunction(removeItem) {
	WriteLog(__func__);
	Player* p; int slot, num;
	if (PyArg_ParseTuple(args, "Kii:removeItem", &p, &slot, &num)) {
		if (CheckisPlayer(p)) {
			p->getContainer()->clearItem(slot, num);
			p->sendInventroy();
		}
	}
	return Py_None;
}
//设置玩家侧边栏
PyAPIFunction(setSidebar) {
	WriteLog(__func__);
	Player* p;
	const char* title;
	const char* data;
	if (PyArg_ParseTuple(args, "Kss:setSidebar", &p, &title, &data)) {
		if (CheckisPlayer(p)) {
			Json::Value j = toJson(data);
			setDisplayObjectivePacket(p, title);
			if (j.isObject()) {
				vector<ScorePacketInfo> info;
				for (auto& x : j.asObject()) {
					ScorePacketInfo o(_scoreboard->createScoreBoardId(x.first),
						x.second.asInt(), x.first);
					info.push_back(o);
				}
				SetScorePacket(p, 0, info);
				return Py_True;
			}
		}
	}
	return Py_False;
}
PyAPIFunction(removeSidebar) {
	WriteLog(__func__);
	Player* p;
	if (PyArg_ParseTuple(args, "K:removeSidebar", &p)) {
		if (CheckisPlayer(p)) {
			setDisplayObjectivePacket(p, "", "");
		}
	}
	return Py_None;
}
//根据坐标设置方块
PyAPIFunction(getBlock) {
	WriteLog(__func__);
	BlockPos bp; int did;
	if (PyArg_ParseTuple(args, "iiii:getBlock", &bp.x, &bp.y, &bp.z, &did)) {
		auto bs = _level->getBlockSource(did);
		if (!bs) {
			cerr << "未知纬度ID:" << did << endl;
			return Py_False;
		}
		auto bl = bs->getBlock(&bp)->getBlockLegacy();
		return Py_BuildValue("{s:s:s:i}",
			"blockname", bl->getBlockName().c_str(),
			"blockid", (int)bl->getBlockItemID()
		);
	}
	return Py_False;
}
PyAPIFunction(setBlock) {
	WriteLog(__func__);
	const char* name;
	BlockPos bp; int did;
	if (PyArg_ParseTuple(args, "siiii:setBlock", &name, &bp.x, &bp.y, &bp.z, &did)) {
		auto bs = _level->getBlockSource(did);
		if (!bs) {
			cerr << "未知纬度ID:" << did << endl;
			return Py_False;
		}
		bs->setBlock(name, bp);
		return Py_True;
	}
	return Py_None;
}
#pragma endregion
#pragma region Function List
static PyMethodDef api_list[]{
Py_Method(getVersion),
Py_Method(logout),
Py_Method(runcmd),
Py_Method(setTimer),
Py_Method(removeTimer),
Py_Method(setListener),
Py_Method(setShareData),
Py_Method(getShareData),
Py_Method(setCommandDescription),
Py_Method(getPlayerList),
Py_Method(sendSimpleForm),
Py_Method(sendModalForm),
Py_Method(sendCustomForm),
Py_Method(transferServer),
Py_Method(getPlayerInfo),
Py_Method(getActorInfo),
Py_Method(getActorInfoEx),
Py_Method(getPlayerPerm),
Py_Method(setPlayerPerm),
Py_Method(addLevel),
Py_Method(setName),
Py_Method(getPlayerScore),
Py_Method(modifyPlayerScore),
Py_Method(talkAs),
Py_Method(runcmdAs),
Py_Method(teleport),
Py_Method(tellraw),
Py_Method(tellrawEx),
Py_Method(setBossBar),
Py_Method(removeBossBar),
Py_Method(getScoreBoardId),
Py_Method(createScoreBoardId),
Py_Method(setDamage),
Py_Method(setServerMotd),
Py_Method(getPlayerItems),
Py_Method(setPlayerItems),
Py_Method(getPlayerHand),
Py_Method(setPlayerHand),
Py_Method(getPlayerArmor),
Py_Method(setPlayerArmor),
Py_Method(getPlayerItem),
Py_Method(setPlayerItem),
Py_Method(getPlayerEnderChests),
Py_Method(setPlayerEnderChests),
Py_Method(addItemEx),
Py_Method(removeItem),
Py_Method(setSidebar),
Py_Method(removeSidebar),
Py_Method(getBlock),
Py_Method(setBlock),
{}
};
static PyModuleDef api_module{ PyModuleDef_HEAD_INIT, "mc", 0, -1, api_list, 0, 0, 0, 0 };
#pragma endregion
static void init() {
	//解释器初始化
	//PyPreConfig cfg;
	//PyPreConfig_InitPythonConfig(&cfg);
	//cfg.utf8_mode = 1;
	//cfg.configure_locale = 0;
	//Py_PreInitialize(&cfg);
	PyImport_AppendInittab("mc", [] { return PyModule_Create(&api_module); }); //增加一个模块
	Py_Initialize();
	PyEval_InitThreads();
	filesystem::directory_iterator files("py");
	for (auto& info : files) {
		auto& path = info.path();
		if (path.extension() == ".py") {
			const string& name = path.stem().u8string();
			print("[BDSpyrunner] loading ", name);
			PyImport_ImportModule(("py." + name).c_str());
			PyErr_Print();
		}
	}
	PyEval_SaveThread();
}
BOOL WINAPI DllMain(HMODULE, DWORD reason, LPVOID) {
	if (reason == 1) {
		//while (1) {
		//	Tag* t = toTag(toJson(R"({})"));
		//	cout << toJson(t).toStyledString() << endl;
		//	t->deCompound();
		//	delete t;
		//}
		ios::sync_with_stdio(false);
		if (!filesystem::exists("py"))
			filesystem::create_directory("py");
		init();
		puts("[BDSpyrunner] 1.2.2 loaded.");
		puts("[BDSpyrunner] 感谢小枫云 http://ipyvps.com 的赞助.");
		WriteLog("Server Started.");
	}
	return 1;
}