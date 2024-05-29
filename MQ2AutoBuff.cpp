
#include <mq/Plugin.h>

#include <vector>

using namespace std;

PreSetup("MQ2AutoBuff");
PLUGIN_VERSION(6.10);

// Max index # from INI - they will be put in order & rewritten, but this is the highest Buff## entry to look for
constexpr const int MAX_BUFFS         = 300;
constexpr const int MAX_NAMES         = 500;
constexpr const int MAX_TOKENS        = 40;
constexpr const int MAX_TOKEN_LEN     = 512;

typedef struct
{
	char t[MAX_TOKENS][MAX_TOKEN_LEN];
} T_TOKENS;

typedef struct req
{
	char sender[50];
	char buff[100];
	char type[10];
	struct req *next;
} *PTR_T_BUFFREQ, T_BUFFREQ;

// changed to using vector
typedef struct
{
	char Keys[120];
	T_TOKENS Tokens;
	char Name[120];
	char Type[120];
} tBuffs;

vector<tBuffs> Buffs;

#define	one_second	40

PTR_T_BUFFREQ	gemwait[NUM_SPELL_GEMS];
PTR_T_BUFFREQ	buffQueue = NULL;
PTR_T_BUFFREQ	endQueue = NULL;
int				BuffCount = 0;
int				waitingToCastGem = -1;
int				maxRetries = 3;
PTR_T_BUFFREQ	waitingToCastPtr = NULL;
bool			waitingToCast = false;
int				pulseCounter = one_second;
bool			autoBuffFlag = false;
bool			bDebug = false;
bool			bAutoBuffGuild = false;
bool			bAutoBuffRaid = false;
bool			bCharControl = false;
bool			bGuildControl = false;
bool			bAutoAdd = false;
bool			Initialized = false;

// Note that AuthorizedGuilds will ALWAYS included a toons own guild
vector<string> AuthorizedNames, AuthorizedGuilds, BlockedNames, BlockedGuilds;

void buffQueueRemove(PTR_T_BUFFREQ ptr);
void buffQueueAdd(char *sender, char *buff, char *type);
int  checkBuffRequest(char *sender, char *message);
void checkBuffQueue();
bool CastBuff(PTR_T_BUFFREQ ptr);
void tokenize(T_TOKENS *tokens, char *list, char *sep);

void AB_Help(PSPAWNINFO pChar, PCHAR szLine);
void AB_Toggle(PSPAWNINFO pChar, PCHAR szLine);
void ABD_Toggle(PSPAWNINFO pChar, PCHAR szLine);
void AB_DoBuff(PSPAWNINFO pChar, PCHAR szLine);
void AB_TargetBuff(PSPAWNINFO pChar, PCHAR szLine);
void AB_Load_INI(PSPAWNINFO pChar, PCHAR szLine);
void AB_DisplayQueue(PSPAWNINFO pChar, PCHAR szLine);
void AB_ClearQueue(PSPAWNINFO pChar, PCHAR szLine);
void AB_CharControl(PSPAWNINFO pChar, PCHAR szLine);
VOID LoadINI();
VOID SaveINI();

int MCEval(const char* zBuffer)
{
	char zOutput[MAX_STRING] = { 0 };
	if (!zBuffer[0])
		return 1;
	strcpy_s(zOutput, zBuffer);
	ParseMacroData(zOutput, MAX_STRING);
	return atoi(zOutput);
}

bool Casting(PCHAR command)
{
	if (GetGameState() == GAMESTATE_INGAME)
	{
		using CastCommandFn = void(*)(SPAWNINFO*, char*);
		CastCommandFn request = (CastCommandFn)GetPluginProc("mq2cast", "CastCommand");
		if (!request)
		{
			WriteChatf("\ao%s\ax::\at[\arMQ2Cast \amplugin is required for AA/SPELL/ITEM casting - these functions are disabled since you do not have the plugin loaded!", PLUGIN_NAME);
			return false;
		}

		request(NULL, command);
		return MCEval("${Cast.Result.Equal[CAST_SUCCESS]}") != 0;
	}
	return false;
}

class MQ2AutoBuffType* pAutoBuffType = 0;

class MQ2AutoBuffType : public MQ2Type
{
public:
	enum AutoBuffMembers {
		Count = 1,
	};

	MQ2AutoBuffType() :MQ2Type("AutoBuff")
	{
		TypeMember(Count);
	}

	virtual bool GetMember(MQVarPtr VarPtr, const char* Member, char* Index, MQTypeVar& Dest) override
	{
		MQTypeMember* pMember = MQ2AutoBuffType::FindMember(Member);
		if (!pMember)
			return false;
		switch ((AutoBuffMembers)pMember->ID)
		{
		case Count:
			Dest.Int = 0;
			PTR_T_BUFFREQ ptr = buffQueue;
			while (ptr)
			{
				Dest.Int++;
				ptr = ptr->next;
			}
			Dest.Type = mq::datatypes::pIntType;
			return true;
		}
		return false;
	}

	bool ToString(MQVarPtr VarPtr, char* Destination)
	{
		strcpy_s(Destination, MAX_STRING, "TRUE");
		return true;
	}
};

bool dataAutoBuff(const char* szName, MQTypeVar& Ret)
{
	Ret.DWord = 1;
	Ret.Type = pAutoBuffType;
	return true;
}

// Check names for authorized users
// Return: true = OK to buff, false = not ok
BOOL CheckNames(PCHAR szName, bool &isGuildMember)
{
	// Valid char data, in game, and that plugin is enabled - or don't buff
	if (!pCharData || gGameState != GAMESTATE_INGAME || !autoBuffFlag)
		return false;
	// Check or own guild if autobuffing our own guild option is enabled, return true if so
	// - unless your own name (in case of auto-add)
	if (pGuild && bAutoBuffGuild && _stricmp(szName, GetCharInfo()->Name)) // skip this check if it's our own name
	{
		if (pGuild->FindMemberByName(szName)) {
			isGuildMember = true;
			// Note:  Need to move this kind of stuff to sub-function, repetitive code.
			// Even if in own guild, if char control is on, need to check for blocked at least...
			if (bCharControl)
			{
				for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
					if (!_stricmp(szName, BlockedNames[Index].c_str()))
						return false;
				}
			}
			return true;
		}
	}
	// Check our raid if autobuffing our raid members option is enabled, return true if so
	// - unless your own name (in case of auto-add)
	if (pRaid && bAutoBuffRaid && _stricmp(szName, GetCharInfo()->Name)) // skip this check if it's our own name
	{
		for (int x = 0; pRaid && x < pRaid->RaidMemberCount; x++)
		{
			if (pRaid && !_stricmp(szName, pRaid->RaidMember->Name))
			{
				// Note:  Need to move this kind of stuff to sub-function, repetitive code.
				// Even if in our raid, if char control is on, need to check for blocked at least...
				if (bCharControl)
				{
					for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
						if (!_stricmp(szName, BlockedNames[Index].c_str()))
							return false;
					}
				}
				return true;
			}
		}
	}
	// After checking bAutoBuffGuild option (always buff own guild members) and
	//   bAutoBuffRaid option (always buff own raid members), check for char and/or guild auth/block control.
	// If neither option is on, just return true (name is ok)
	if (!bCharControl && !bGuildControl)
		return true;
	// if option is on, check char names for auth/block first
	if (bCharControl)
	{
		// Note:  Need to move this kind of stuff to sub-function, repetitive code.
		for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
			if (!_stricmp(szName, BlockedNames[Index].c_str()))
				return false;
		}
		for (unsigned int Index = 0; Index < AuthorizedNames.size(); Index++) {
			if (!_stricmp(szName, AuthorizedNames[Index].c_str()))
				return true;
		}
	}
	// If option enabled, check for any blocked or authorized guilds
	if (bGuildControl)
	{
		PSPAWNINFO tSpawn = (PSPAWNINFO)GetSpawnByName(szName);
		if (tSpawn && tSpawn->SpawnID > 0 && tSpawn->GuildID != 0 && tSpawn->GuildID != -1)
		{
			char szGuildName[MAX_STRING] = { 0 };
			const char* szGuild = GetGuildByID(tSpawn->GuildID);
			strcpy_s(szGuildName, szGuild);
			for (unsigned int Index = 0; Index < BlockedGuilds.size(); Index++) {
				if (!_stricmp(szGuildName, BlockedGuilds[Index].c_str()))
					return false;
			}
			for (unsigned int Index = 0; Index < AuthorizedGuilds.size(); Index++) {
				if (!_stricmp(szGuildName, AuthorizedGuilds[Index].c_str()))
					return true;
			}
		}
	}
	// Fell through, so not ok to buff (all other possibilities have been checked by now)
	return false;
}

// MMOLoader: Active login, turn on commands, etc.
void PluginOn()
{
	if (mmo.Active)
		return;
	mmo.Active = true;
	AddCommand("/abhelp", AB_Help, 0, 0, 1);
	AddCommand("/ab", AB_Toggle, 0, 0, 1);
	AddCommand("/abd", ABD_Toggle, 0, 0, 1);
	AddCommand("/db", AB_DoBuff, 0, 0, 1);
	AddCommand("/tb", AB_TargetBuff, 0, 0, 1);
	AddCommand("/readini", AB_Load_INI, 0, 0, 1);
	AddCommand("/dq", AB_DisplayQueue, 0, 0, 1);
	AddCommand("/cq", AB_ClearQueue, 0, 0, 1);
	AddCommand("/abc", AB_CharControl, 0, 0, 1);
	pAutoBuffType = new MQ2AutoBuffType;
	AddMQ2Data("AutoBuff", dataAutoBuff);
}

// MMOLoader: Inactive login, turn off commands, etc.
void PluginOff()
{
	if (!mmo.Active)
		return;
	mmo.Active = false;
	RemoveMQ2Data("AutoBuff");
	delete pAutoBuffType;
	// clear buff request Queue
	while (autoBuffFlag && buffQueue)
		buffQueueRemove(buffQueue);
	RemoveCommand("/abhelp");
	RemoveCommand("/ab");
	RemoveCommand("/abd");
	RemoveCommand("/db");
	RemoveCommand("/tb");
	RemoveCommand("/readini");
	RemoveCommand("/dq");
	RemoveCommand("/cq");
	RemoveCommand("/abc");
}

// Called once directly after initialization, and then every time the gamestate changes
PLUGIN_API VOID SetGameState(DWORD GameState)
{
	CHAR szBuffer[MAX_STRING] = { 0 };
	if (!mmo.Active)
		return;
	if (gGameState == GAMESTATE_INGAME)
	{
		for (int i = 0; i < NUM_SPELL_GEMS; i++)
			gemwait[i] = NULL;

		buffQueue = NULL;
		endQueue = NULL;

		if (GetCharInfo())
		{
			sprintf_s(INIFileName, "%s\\%s_%s.ini", gPathConfig, EQADDR_SERVERNAME, GetCharInfo()->Name);
			AB_Load_INI(NULL, NULL);
			LoadINI();
			autoBuffFlag = true;
			if (!Initialized) {
				Initialized = true;
				if (bAutoAdd) {
					strcpy_s(szBuffer, GetCharInfo()->Name);
					_strlwr_s(szBuffer);
					for (unsigned int Index = 0; Index < AuthorizedNames.size(); Index++) {
						if (!_stricmp(szBuffer, AuthorizedNames[Index].c_str())) {
							return;
						}
					}
					AuthorizedNames.push_back(szBuffer);
					WriteChatf("\ar%s: \at'%s' automatically added.", PLUGIN_NAME, GetCharInfo()->Name);
					SaveINI();
				}
			}
		}
	}
	else
	{
		autoBuffFlag = false;
		if (GameState != GAMESTATE_LOGGINGIN) {
			if (Initialized) {
				Initialized = 0;
			}
		}
	}
}

// Called once, when the plugin is to initialize
PLUGIN_API VOID InitializePlugin(VOID)
{
	if (!MMOAllowedPlugin(mqplugin::ghPluginModule, PLUGIN_NAME))
	{
		char szBuffer[MAX_STRING] = { 0 };
		sprintf_s(szBuffer, "/timed 10 /plugin %s unload noauto", PLUGIN_NAME);
		EzCommand(szBuffer);
		return;
	}
	MMORequiredAccess = GetRequiredAccess();
	if (LOK(MMORequiredAccess))
	{
		PluginOn();
		if (gGameState == GAMESTATE_INGAME)
			SetGameState(GAMESTATE_INGAME);
	}
	else
		WriteChatf("\ar%s \aysubscription is required to use \ag%s", MMOAccessName[MMORequiredAccess], PLUGIN_NAME);
}

// Called once, when the plugin is to shutdown
PLUGIN_API VOID ShutdownPlugin(VOID)
{
	PluginOff();
}

// Called after entering a new zone (client gets end zone opcode)
PLUGIN_API VOID OnEndZone(VOID)
{
	if (LOK(MMORequiredAccess))
		PluginOn();
	else
		PluginOff();
}

// This is called every time MQ pulses
PLUGIN_API VOID OnPulse(VOID)
{
	if (gGameState != GAMESTATE_INGAME || !mmo.Active || !autoBuffFlag)
		return;
	// check give it 10 pulses before we try to cast.
	if (pulseCounter-- > 0)
		return;
	checkBuffQueue();
	pulseCounter += one_second;
}

// This is called every time EQ shows a line of chat with CEverQuest::dsp_chat,
// but after MQ filters and chat events are taken care of.
PLUGIN_API DWORD OnIncomingChat(PCHAR Line, DWORD Color)
{
	char szTemp[MAX_STRING], sender[250], message[MAX_STRING], szDisp[MAX_STRING] = { 0 }, szChar[5], *p, *nt;
	if (!mmo.Active || !autoBuffFlag || !pCharData || gGameState != GAMESTATE_INGAME)
		return 0;
	DebugSpewAlways("%s::OnIncomingChat(%s)", PLUGIN_NAME, Line);
	if (strstr(Line, " tells you, '") || strstr(Line, " tells you, in "))
	{
		bool isGuildMember = false;
		bool bStart = false;
		strcpy_s(szTemp, Line);
		for (unsigned int XX = 0; XX < strlen(szTemp); XX++)
		{
			if (isprint(szTemp[XX]))
			{
				sprintf_s(szChar, "%c", szTemp[XX]);
				strcat_s(szDisp, szChar);
			}
			else if (szTemp[XX] == 0x12)
			{
				if (bStart)
				{
					bStart = false;
				}
				else
				{
					bStart = true;
					XX++;
				}
			}
		}
		if (bDebug)
			WriteChatf("%s::OnIncomingChat: Filtered to >>%s<<", PLUGIN_NAME, szDisp);
		p = strtok_s(szDisp, " ", &nt); // szDisp is printable only, find first space (which returns toon name)
		if (p)
		{
			strcpy_s(sender, p);
			if (CheckNames(sender, isGuildMember))
			{
				if (bDebug)
					WriteChatf("%s::OnIncomingChat: Determined name >>%s<<", PLUGIN_NAME, sender);
				p = strtok_s(NULL, "'", &nt); // find opening apost for message, we will discard
				if (p)
				{
					p = strtok_s(NULL, "'", &nt); // find closing apost for message, we will use
					if (p)
					{
						strcpy_s(message, p);
						if (bDebug)
							WriteChatf("%s::OnIncomingChat: sender=>>%s<< message=>>%s<<", PLUGIN_NAME, sender, message);
						checkBuffRequest(sender, message);
					}
					else
					{
						if (bDebug)
							WriteChatf("%s::OnIncomingChat: Could not find message end", PLUGIN_NAME);
					}
				}
				else
				{
					if (bDebug)
						WriteChatf("%s::OnIncomingChat: Could not find message start", PLUGIN_NAME);
				}
			}
		}
	}
	return 0;
}

// Load INI values for authorized names only
VOID LoadINI()
{
	int KeyIndex;
	char KeyName[MAX_STRING], KeyValue[MAX_STRING];
	bool Found;

	if (GetPrivateProfileString(PLUGIN_NAME, "UseAuthorizedNames", NULL, KeyName, MAX_STRING, INIFileName)) {
		if (!_stricmp(KeyName, "on"))
			bCharControl = true;
		else
			bCharControl = false;
	}
	if (GetPrivateProfileString(PLUGIN_NAME, "UseAuthorizedGuilds", NULL, KeyName, MAX_STRING, INIFileName)) {
		if (!_stricmp(KeyName, "on"))
			bGuildControl = true;
		else
			bGuildControl = false;
	}
	if (GetPrivateProfileString(PLUGIN_NAME, "AutoAdd", NULL, KeyName, MAX_STRING, INIFileName)) {
		if (!_stricmp(KeyName, "on"))
			bAutoAdd = true;
		else
			bAutoAdd = false;
	}
	if (GetPrivateProfileString(PLUGIN_NAME, "AutoBuffGuild", NULL, KeyName, MAX_STRING, INIFileName)) {
		if (!_stricmp(KeyName, "on"))
			bAutoBuffGuild = true;
		else
			bAutoBuffGuild = false;
	}
	if (GetPrivateProfileString(PLUGIN_NAME, "AutoBuffRaid", NULL, KeyName, MAX_STRING, INIFileName)) {
		if (!_stricmp(KeyName, "on"))
			bAutoBuffRaid = true;
		else
			bAutoBuffRaid = false;
	}
	maxRetries = GetPrivateProfileInt(PLUGIN_NAME, "MaxRetries", 3, INIFileName);

	AuthorizedNames.clear();
	BlockedNames.clear();
	AuthorizedGuilds.clear();
	BlockedGuilds.clear();
	KeyIndex = 0;
	do {
		char szSection[MAX_STRING] = { 0 };
		sprintf_s(szSection, "%s_Authorized_Names", PLUGIN_NAME);
		sprintf_s(KeyName, "Name%d", KeyIndex);
		GetPrivateProfileString(szSection, KeyName, NULL, KeyValue, MAX_STRING, INIFileName);
		if (strlen(KeyValue) > 0) {
			_strlwr_s(KeyValue);
			Found = false;
			for (unsigned int Index = 0; Index < AuthorizedNames.size(); Index++) {
				if (!_stricmp(KeyValue, AuthorizedNames[Index].c_str()))
					Found = true;
			}
			if (!Found)
				AuthorizedNames.push_back(KeyValue);
		}
		KeyIndex++;
	} while (strlen(KeyValue) > 0 || KeyIndex < MAX_NAMES);
	KeyIndex = 0;
	do {
		char szSection[MAX_STRING] = { 0 };
		sprintf_s(szSection, "%s_Blocked_Names", PLUGIN_NAME);
		sprintf_s(KeyName, "Name%d", KeyIndex);
		GetPrivateProfileString(szSection, KeyName, NULL, KeyValue, MAX_STRING, INIFileName);
		if (strlen(KeyValue) > 0) {
			_strlwr_s(KeyValue);
			Found = false;
			for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
				if (!_stricmp(KeyValue, BlockedNames[Index].c_str()))
					Found = true;
			}
			if (!Found)
				BlockedNames.push_back(KeyValue);
		}
		KeyIndex++;
	} while (strlen(KeyValue) > 0 || KeyIndex < MAX_NAMES);
	KeyIndex = 0;
	do {
		char szSection[MAX_STRING] = { 0 };
		sprintf_s(szSection, "%s_Authorized_Guilds", PLUGIN_NAME);
		sprintf_s(KeyName, "Name%d", KeyIndex);
		GetPrivateProfileString(szSection, KeyName, NULL, KeyValue, MAX_STRING, INIFileName);
		if (strlen(KeyValue) > 0) {
			_strlwr_s(KeyValue);
			Found = false;
			for (unsigned int Index = 0; Index < AuthorizedGuilds.size(); Index++) {
				if (!_stricmp(KeyValue, AuthorizedGuilds[Index].c_str()))
					Found = true;
			}
			if (!Found)
				AuthorizedGuilds.push_back(KeyValue);
		}
		KeyIndex++;
	} while (strlen(KeyValue) > 0 || KeyIndex < MAX_NAMES);
	KeyIndex = 0;
	do {
		char szSection[MAX_STRING] = { 0 };
		sprintf_s(szSection, "%s_Blocked_Guilds", PLUGIN_NAME);
		sprintf_s(KeyName, "Name%d", KeyIndex);
		GetPrivateProfileString(szSection, KeyName, NULL, KeyValue, MAX_STRING, INIFileName);
		if (strlen(KeyValue) > 0) {
			_strlwr_s(KeyValue);
			Found = false;
			for (unsigned int Index = 0; Index < BlockedGuilds.size(); Index++) {
				if (!_stricmp(KeyValue, BlockedGuilds[Index].c_str()))
					Found = true;
			}
			if (!Found)
				BlockedGuilds.push_back(KeyValue);
		}
		KeyIndex++;
	} while (strlen(KeyValue) > 0 || KeyIndex < MAX_NAMES);
}

// Save INI values for authorized names only
VOID SaveINI()
{
	char KeyName[MAX_STRING];
	WritePrivateProfileString(PLUGIN_NAME, "UseAuthorizedNames", bCharControl ? "on" : "off", INIFileName);
	WritePrivateProfileString(PLUGIN_NAME, "UseAuthorizedGuilds", bGuildControl ? "on" : "off", INIFileName);
	WritePrivateProfileString(PLUGIN_NAME, "AutoBuffGuild", bAutoBuffGuild ? "on" : "off", INIFileName);
	WritePrivateProfileString(PLUGIN_NAME, "AutoBuffRaid", bAutoBuffRaid ? "on" : "off", INIFileName);
	WritePrivateProfileString(PLUGIN_NAME, "AutoAdd", bAutoAdd ? "on" : "off", INIFileName);
	char szTemp[MAX_STRING] = { 0 };
	_itoa_s(maxRetries, szTemp, 10);
	WritePrivateProfileString(PLUGIN_NAME, "MaxRetries", szTemp, INIFileName);
	char szSection[MAX_STRING] = { 0 };
	sprintf_s(szSection, "%s_Authorized_Names", PLUGIN_NAME);
	WritePrivateProfileSection(szSection, "", INIFileName);
	for (unsigned int Index = 0; Index < AuthorizedNames.size(); Index++) {
		sprintf_s(KeyName, "Name%d", Index + 1);
		WritePrivateProfileString(szSection, KeyName, AuthorizedNames[Index].c_str(), INIFileName);
	}
	sprintf_s(szSection, "%s_Blocked_Names", PLUGIN_NAME);
	WritePrivateProfileSection(szSection, "", INIFileName);
	for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
		sprintf_s(KeyName, "Name%d", Index + 1);
		WritePrivateProfileString(szSection, KeyName, BlockedNames[Index].c_str(), INIFileName);
	}
	sprintf_s(szSection, "%s_Authorized_Guilds", PLUGIN_NAME);
	WritePrivateProfileSection(szSection, "", INIFileName);
	for (unsigned int Index = 0; Index < AuthorizedGuilds.size(); Index++) {
		sprintf_s(KeyName, "Name%d", Index + 1);
		WritePrivateProfileString(szSection, KeyName, AuthorizedGuilds[Index].c_str(), INIFileName);
	}
	sprintf_s(szSection, "%s_Blocked_Guilds", PLUGIN_NAME);
	WritePrivateProfileSection(szSection, "", INIFileName);
	for (unsigned int Index = 0; Index < BlockedGuilds.size(); Index++) {
		sprintf_s(KeyName, "Name%d", Index + 1);
		WritePrivateProfileString(szSection, KeyName, BlockedGuilds[Index].c_str(), INIFileName);
	}
}

// show a usage message
void AB_Help(PSPAWNINFO pChar, PCHAR szLine)
{
	WriteChatf("\ar%s\ax::\atCommands (Version %1.2f):", PLUGIN_NAME, MQ2Version);
	WriteChatf("\ag/abhelp \ax- \aydisplay this message");
	WriteChatf("\ag/ab \ax- \aypause/restart processing of buff queue and buff requests");
	WriteChatf("\ag/db <name> <buff keyword> \ax- \ayadd buff to queue");
	WriteChatf("\ag/tb <buff keyword> \ax- \ayadd buff to queue for target");
	WriteChatf("\ag/readini \ax- \ayreload the INI file");
	WriteChatf("\ag/dq \ax- \aylist buffs in the queue");
	WriteChatf("\ag/cq \ax- \ayclear the buff queue");
	WriteChatf("\ag/abc \ax- \aycontrols and lists authorized/blocked char/guild name options");
}

// Toggle AutoBuff on/off
void AB_Toggle(PSPAWNINFO pChar, PCHAR szLine)
{
	if (autoBuffFlag)
	{
		autoBuffFlag = false;
		WriteChatf("\ar%s\ax::\ayPAUSED!", PLUGIN_NAME);
	}
	else
	{
		autoBuffFlag = true;
		WriteChatf("%s\ax::\ayRESTARTED!", PLUGIN_NAME);
	}

}

// Toggle Debug
void ABD_Toggle(PSPAWNINFO pChar, PCHAR szLine)
{
	if (bDebug)
	{
		bDebug = false;
		WriteChatf("\ar%s\ax::\ayDebug off.", PLUGIN_NAME);
	}
	else
	{
		bDebug = true;
		WriteChatf("\ar%s\ax::\ayDebug on.", PLUGIN_NAME);
	}

}

// Add a buff request to the queue
void AB_DoBuff(PSPAWNINFO pChar, PCHAR szLine)
{
	char szWho[MAX_STRING];
	char szBuff[MAX_STRING];

	if (autoBuffFlag)
	{
		GetArg(szWho, szLine, 1);
		GetArg(szBuff, szLine, 2);

		if (strlen(szWho) && strlen(szBuff))
		{
			if (!checkBuffRequest(szWho, szBuff))
			{
				WriteChatf("\arMQ2AB\ax::\arBuff \ay[%s]\ar not found!", szBuff);
			}
		}
		else
			WriteChatf("\ar%s\ax::\apUsage: /db <target> <buff>", PLUGIN_NAME);
	}
}

// Add a buff request to the queue for who player has targeted
void AB_TargetBuff(PSPAWNINFO pChar, PCHAR szLine)
{
	char szWho[MAX_STRING];
	char szBuff[MAX_STRING];

	if (autoBuffFlag)
	{
		if (pTarget)
			strcpy_s(szWho, pTarget->DisplayedName);
		else
		{
			WriteChatf("\ar%s\ax::\ayYou do not have a target.", PLUGIN_NAME);
			return;
		}
		strcpy_s(szBuff, szLine);
		if (strlen(szWho) && strlen(szBuff))
		{
			if (!checkBuffRequest(szWho, szBuff))
			{
				WriteChatf("\ar%s\ax::\arBuff \ay[%s]\ar not found!", PLUGIN_NAME, szBuff);
			}
		}
		else
			WriteChatf("\ar%s\ax::\ap Usage: /tb <buff>", PLUGIN_NAME);
	}
}

// load up the INI file
void AB_Load_INI(PSPAWNINFO pChar, PCHAR szLine)
{
	char szTemp[MAX_STRING];
	char name[0x40];
	tBuffs dBuff;

	BuffCount = 0;
	Buffs.clear();
	GetPrivateProfileString(PLUGIN_NAME, "AutoBuff", "NULL", szTemp, MAX_STRING, INIFileName);
	if (strstr(szTemp, "NULL"))
	{
		WriteChatf("\ar%s\ax::\aySection not found in %s, creating %s section.", PLUGIN_NAME, INIFileName, PLUGIN_NAME);
		WritePrivateProfileString(PLUGIN_NAME, "AutoBuff", "on", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "MaxRetries", "3", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "Keys1", "keyword1,keyword2,keyword3", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "Name1", "Spell Name", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "Type1", "gem1", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "Keys2", "keyword1,keyword2,keyword3", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "Name2", "Another Spell Name", INIFileName);
		WritePrivateProfileString(PLUGIN_NAME, "Type2", "gem5", INIFileName);
	}
	else
	{
		maxRetries = GetPrivateProfileInt(PLUGIN_NAME, "MaxRetries", 3, INIFileName);
		for (int x = 0; x < MAX_BUFFS; x++)
		{
			sprintf_s(name, "Keys%d", x + 1);
			GetPrivateProfileString(PLUGIN_NAME, name, "NULL", szTemp, MAX_STRING, INIFileName);
			if (strstr(szTemp, "NULL"))
				continue;
			else
			{
				strcpy_s(dBuff.Keys, szTemp);
				tokenize(&dBuff.Tokens, dBuff.Keys, ",");
				sprintf_s(name, "Name%d", x + 1);
				GetPrivateProfileString(PLUGIN_NAME, name, "NULL", szTemp, MAX_STRING, INIFileName);
				strcpy_s(dBuff.Name, szTemp);
				sprintf_s(name, "Type%d", x + 1);
				GetPrivateProfileString(PLUGIN_NAME, name, "NULL", szTemp, MAX_STRING, INIFileName);
				strcpy_s(dBuff.Type, szTemp);
				Buffs.push_back(dBuff);
				BuffCount++;
			}
		}
		if (pChar)
			WriteChatf("\ar%s\ax:: \ag%s section reloaded, %d buff entries.", PLUGIN_NAME, INIFileName, BuffCount);
	}
}

// Display what buffs are in the queue
void AB_DisplayQueue(PSPAWNINFO pChar, PCHAR szLine)
{
	int i = 1;

	WriteChatf("\axBuffee     Spell           Type");
	WriteChatf("\ax---------------------------------------");
	PTR_T_BUFFREQ ptr = buffQueue;
	while (ptr)
	{
		WriteChatf("\ax%02d %s %s %s", i++, ptr->sender, ptr->buff, ptr->type);
		ptr = ptr->next;
	}
	WriteChatf("\axEnd of Queue ------------------");
}

// clear out the buff queue
void AB_ClearQueue(PSPAWNINFO pChar, PCHAR szLine)
{
	while (buffQueue)
		buffQueueRemove(buffQueue);
	waitingToCastPtr = NULL;
	for (int i = 0; i < NUM_SPELL_GEMS; i++)
		gemwait[i] = NULL;
	WriteChatf("\ar%s\ax::\ayQueue cleared", PLUGIN_NAME);
}

// Add a buff request to the queue
void buffQueueAdd(char *sender, char *buff, char *type)
{
	PTR_T_BUFFREQ req;

	if (bDebug)
		WriteChatf("\ar%s\ax::\atbuffQueueAdd: sender='%s', buff='%s', type='%s'", PLUGIN_NAME, sender, buff, type);
	req = (PTR_T_BUFFREQ)malloc(sizeof(T_BUFFREQ));
	req->next = NULL;
	strcpy_s(req->sender, sender);
	strcpy_s(req->buff, buff);
	strcpy_s(req->type, type);

	if (!buffQueue)
	{
		buffQueue = req;
		endQueue = req;
	}
	else
	{
		endQueue->next = req;
		endQueue = req;
	}
}

// Remove a buff request from the queue
void buffQueueRemove(PTR_T_BUFFREQ ptr)
{
	if (ptr == buffQueue)
	{
		buffQueue = buffQueue->next;
		if (!buffQueue)
			endQueue = NULL;
	}
	else
	{
		PTR_T_BUFFREQ p = buffQueue;
		while (p->next != ptr)
			p = p->next;
		p->next = ptr->next;
		if (endQueue == ptr)
			endQueue = p;
	}
	free(ptr);
}

// Check the buff queue for any waiting entries
void checkBuffQueue()
{
	PCHARINFO pCharInfo = GetCharInfo();
	if (!pCharInfo)
		return;
	if (!pCharInfo->pSpawn)
		return;

	//  are we already casting?
	if (pCharInfo->pSpawn->CastingData.SpellID != -1)
		return;

	// is the casting window up?
	if (pCastingWnd && pCastingWnd->IsVisible() == 1)
		return;

	// are we moving?
	if (gbMoving)
		return;

	// are we stunned?
	if (pCharInfo->Stunned)
		return;

	// Check to see if memmed spells we are waiting on are ready
	for (int i = 0; i < NUM_SPELL_GEMS; i++)
	{
		if (gemwait[i] &&
			pCastSpellWnd->SpellSlots[i] &&
			pCastSpellWnd->SpellSlots[i]->spellstate != 1)
		{
			CastBuff(gemwait[i]);
			gemwait[i] = NULL;
			return;
		}
	}

	// look for next buff request
	PTR_T_BUFFREQ ptr = buffQueue;
	while (ptr)
	{
		bool skip = false;

		// does this request want a gem we are already waiting for
		if (strstr(ptr->type, "gem"))
		{
			int gem = atoi(ptr->type + 3);
			if (gem > 0 && gem < NUM_SPELL_GEMS + 1 && gemwait[gem - 1])
				skip = true;
		}

		if (!skip)
			if (CastBuff(ptr))
				return;

		ptr = ptr->next;
	}
}

// Retrieve spell/item info
EQ_Spell* getSpell(char *spellName, char *type)
{
	PcProfile* pProfile = GetPcProfile();

	if (_stricmp(type, "item"))
	{
		return GetSpellByName(spellName);
	}

	for (const ItemPtr& pItem : pProfile->GetInventory())
	{
		if (pItem && !_stricmp(spellName, pItem->GetName()))
			return GetSpellByID(pItem->GetItemDefinition()->Clicky.SpellID);
	}

	return nullptr;
}

// Cast the spell/item
bool CastBuff(PTR_T_BUFFREQ ptr)
{
	int gem = 0, memgem = 0;
	bool spellFound = false;
	char szTemp[MAX_STRING] = { 0 }, szTemp1[MAX_STRING] = { 0 };
	PCHARINFO pCharInfo = GetCharInfo();
	PcProfile* pProfile = GetPcProfile();
	FLOAT spellRange = 0;
	char szBuffer[MAX_STRING];
	if (!pCharInfo || !pProfile)
		return false;
	if (!pCharInfo->pSpawn)
		return false;
	// find sender
	PSPAWNINFO pSpawn = (PSPAWNINFO)pSpawnList;
	while (pSpawn)
	{
		if (!_stricmp(pSpawn->DisplayedName, ptr->sender))
			break;
		pSpawn = pSpawn->pNext;
	}
	if (!pSpawn)
	{
		// sender not in zone, remove request
		if (bDebug) {
			for (unsigned int ii = 0; ii < strlen(ptr->sender); ii++)
			{
				sprintf_s(szTemp1, "0x%02X ", ptr->sender[ii]);
				strcat_s(szTemp, szTemp1);
			}
			WriteChatf("\arMQ2AutoDebuff\ax::\atCastBuff: sender = %s", szTemp);
		}
		WriteChatf("\ar%s\ax::\ay%s is not in the zone, ignoring request", PLUGIN_NAME, ptr->sender);
		buffQueueRemove(ptr);
		return false;
	}
	// make sure we have a valid spell to cast
	PSPELL pSpell = getSpell(ptr->buff, ptr->type);
	if (!pSpell)
	{
		// spell not found, remove request
		WriteChatf("\ar%s\ax::\aySpell/item/AA not found [%s]", PLUGIN_NAME, ptr->buff);
		buffQueueRemove(ptr);
		return false;
	}
	// check to see if target is in range
	if (pSpell->TargetType == 41 /* Group v2 */)
		spellRange = 1.18f * pSpell->AERange;
	else
		spellRange = 1.18f * pSpell->Range;
	if (DistanceToSpawn3D(pSpawn, pCharInfo->pSpawn) > spellRange)
		return false;
	// target sender
	pTarget = pSpawn;
	// if its an item then just click it
	if (!_stricmp(ptr->type, "item"))
	{
		WriteChatf("\ar%s\ax::\ayCasting: %s on >> %s <<", PLUGIN_NAME, ptr->buff, pSpawn->Name);
		// cast clicky
		sprintf_s(szBuffer, "\"%s\"|item", ptr->buff);
		Casting(szBuffer);
		buffQueueRemove(ptr);
		return true;
	}
	if (!_stricmp(ptr->type, "alt"))
	{
		WriteChatf("\ar%s\ax::\ayCasting: %s on >> %s <<", PLUGIN_NAME, ptr->buff, pSpawn->Name);
		sprintf_s(szBuffer, "\"%s\"|alt", ptr->buff);
		Casting(szBuffer);
		buffQueueRemove(ptr);
		return true;
	}
	// check mana for enough mana to cast
	if (pProfile->Mana < pSpell->ManaCost)
		return true;
	// check to see if spell is memorized
	gem = 0;
	for (int nGem = 0; nGem < NUM_SPELL_GEMS; nGem++)
	{
		if (pProfile->MemorizedSpells[nGem] != 0xFFFFFFFF)
		{
			const char* SpellName = GetSpellNameByID(pProfile->MemorizedSpells[nGem]);
			if (!_stricmp(ptr->buff, SpellName))
			{
				spellFound = true;
				gem = nGem;
			}
		}
	}
	if (!spellFound)
	{
		// memorize the spell
		memgem = atoi(ptr->type + 3);
		sprintf_s(szBuffer, "%d \"%s\"", memgem, ptr->buff);
		MemSpell(pCharInfo->pSpawn, szBuffer);
		gemwait[memgem - 1] = ptr;
		pulseCounter += one_second * 3; // give spell a chance to be memorized
		return true;
	}
	// make sure spells are ready to cast
	if (!pCastSpellWnd->SpellSlots[gem] || pCastSpellWnd->SpellSlots[gem]->spellstate == 1)
		return false;
	WriteChatf("\ar%s\ax::\ayCasting: %s on >> %s <<", PLUGIN_NAME, ptr->buff, pSpawn->Name);
	// cast buff
	sprintf_s(szBuffer, "\"%s\"|gem%d -maxtries|%d", ptr->buff, gem + 1, maxRetries);
	Casting(szBuffer);
	// FIXME: 1) how can I confirm spell was cast?
	//        2) what do I do about fizzle?
	//        3) what if we interrupt cast?
	//        4) what if sender moves out of range?
	buffQueueRemove(ptr);
	return true;
}

// Function to tokenize a string
void tokenize(T_TOKENS *tokens, char *list, char *sep)
{
	char szTemp[MAX_STRING];
	char *token, *nt;
	int i = 0;
	strcpy_s(szTemp, list);
	token = strtok_s(szTemp, sep, &nt);
	for (i = 0; token && i < MAX_TOKENS - 1; i++)
	{
		if (token[strlen(token) - 1] == '\'') token[strlen(token) - 1] = 0;
		strncpy_s(tokens->t[i], token, min((int)strlen(token), MAX_TOKEN_LEN - 1));
		tokens->t[i][min((int)strlen(token), MAX_TOKEN_LEN - 1)] = 0;
		token = strtok_s(NULL, sep, &nt);
	}
	tokens->t[i][0] = 0;
}

T_TOKENS mesg_token;
T_TOKENS name_token;
T_TOKENS type_token;

// Check if a buff needs to be added to queue
int checkBuffRequest(char *sender, char *message)
{
	int j, k, l;
	int count = 0;
	if (bDebug)
		WriteChatf("\ar%s\ax::\atcheckBuffRequest: sender='%s', message='%s'", PLUGIN_NAME, sender, message);
	tokenize(&mesg_token, message, " /?,&.!;:'");
	// For each Buff specified in the INI file
	for (unsigned int i = 0; i < Buffs.size(); i++)
	{
		tBuffs& BuffRef = Buffs[i];
		if (bDebug)
			WriteChatf("\ar%s\ax::\atcheckBuffRequest: Processing buff entry %d/%d", PLUGIN_NAME, i + 1, Buffs.size());
		// For each word in the tell message
		for (j = 0; j < MAX_TOKENS && mesg_token.t[j][0]; j++)
		{
			if (bDebug)
				WriteChatf("\ar%s\ax::\atcheckBuffRequest: Processing word %d, '%s'", PLUGIN_NAME, j + 1, mesg_token.t[j]);
			// For each keyword in the buff keys
			for (k = 0; k < MAX_TOKENS && BuffRef.Tokens.t[k][0]; k++)
			{
				if (bDebug)
					WriteChatf("\ar%s\ax::\atcheckBuffRequest: Processing keyword %d, '%s'", PLUGIN_NAME, k + 1, BuffRef.Tokens.t[k]);
				// compare word in tell message to keyword in buff keys
				if (!_stricmp(mesg_token.t[j], BuffRef.Tokens.t[k]))
				{
					if (bDebug)
						WriteChatf("\ar%s\ax::\atcheckBuffRequest: Buff match '%s' == '%s'", PLUGIN_NAME, mesg_token.t[j], BuffRef.Tokens.t[k]);
					// Buff Name can be a list of buffs
					tokenize(&name_token, BuffRef.Name, ",");
					tokenize(&type_token, BuffRef.Type, ",");
					for (l = 0; l < MAX_TOKENS && name_token.t[l][0]; l++)
					{
						if (bDebug)
							WriteChatf("\ar%s\ax::\atcheckBuffRequest: Processing list of buffs %ld, name_token='%s', type_token='%s'", PLUGIN_NAME, l + 1, name_token.t[l], type_token.t[l]);
						buffQueueAdd(sender, name_token.t[l], type_token.t[l]);
						count++;
					}
				}
			}
		}
	}
	return(count);
}

void lowerCase(string& strToConvert)
{
	for (unsigned int i = 0; i < strToConvert.length(); i++)
	{
		strToConvert[i] = tolower(strToConvert[i]);
	}
}

char *MakeProperCase(string bStrParm)
{
	bool thisWordCapped = false;
	static char convertedString[MAX_STRING];
	string strToConvert = bStrParm;
	convertedString[0] = 0;
	lowerCase(strToConvert);
	for (unsigned int i = 0; i < strToConvert.length(); i++)
	{
		if ((ispunct(strToConvert[i])) || (isspace(strToConvert[i])))
			thisWordCapped = false;
		if ((thisWordCapped == false) && (isalpha(strToConvert[i])))
		{
			strToConvert[i] = toupper(strToConvert[i]);
			thisWordCapped = true;
		}
	}
	strcpy_s(convertedString, strToConvert.c_str());
	return(convertedString);
}

VOID AB_CharControl(PSPAWNINFO pChar, PCHAR szLine)
{
	char szTemp[MAX_STRING], szBuffer[MAX_STRING];

	GetArg(szTemp, szLine, 1);
	// Turn buff auth control on
	if (!_stricmp(szTemp, "on")) {
		bCharControl = true;
		WriteChatf("\at%s\ax: Use Authorized/Blocked Names Lists now \agON\ax.", PLUGIN_NAME);
	}
	// Turn buff auth control off
	else if (!_stricmp(szTemp, "off")) {
		bCharControl = false;
		WriteChatf("\at%s\ax: Use Authorized/Blocked Names Lists now \ayOFF\ax.", PLUGIN_NAME);
	}
	// Turn buff auth control on for guilds
	else if (!_stricmp(szTemp, "gon")) {
		bGuildControl = true;
		WriteChatf("\at%s\ax: Use Authorized/Blocked Guilds List now \agON\ax.", PLUGIN_NAME);
	}
	// Turn buff auth control off for guilds
	else if (!_stricmp(szTemp, "goff")) {
		bGuildControl = false;
		WriteChatf("\at%s\ax: Use Authorized/Blocked Guilds List now \ayOFF\ax.", PLUGIN_NAME);
	}
	// Toggle buff auto-auth guild
	else if (!_stricmp(szTemp, "guild")) {
		bAutoBuffGuild = !bAutoBuffGuild;
		WriteChatf("\at%s\ax: Always Authorize \ayGuild\ax now %s", bAutoBuffGuild ? "\agOn" : "\arOff", PLUGIN_NAME);
	}
	// Toggle buff auto-auth raid
	else if (!_stricmp(szTemp, "raid")) {
		bAutoBuffRaid = !bAutoBuffRaid;
		WriteChatf("\at%s\ax: Always Authorize \ayRaid\ax now %s", bAutoBuffRaid ? "\agOn" : "\arOff", PLUGIN_NAME);
	}
	// Toggle autoadd
	else if (!_stricmp(szTemp, "auto")) {
		bAutoAdd = !bAutoAdd;
		WriteChatf("\at%s\ax: Auto Add Authorized User now %s", PLUGIN_NAME, bAutoAdd ? "\agOn" : "\arOff");
	}
	// Reload .ini
	else if (!_stricmp(szTemp, "load")) {
		LoadINI();
		WriteChatf("\at%s\ax: \aoINI loaded.", PLUGIN_NAME);
	}
	// Save current settings & names
	else if (!_stricmp(szTemp, "save")) {
		SaveINI();
		WriteChatf("\at%s\ax: \aoINI saved.", PLUGIN_NAME);
	}
	// Add name
	else if (!_stricmp(szTemp, "add")) {
		GetArg(szBuffer, szLine, 2);
		if (strlen(szBuffer) > 0) {
			_strlwr_s(szBuffer);
			for (unsigned int Index = 0; Index < AuthorizedNames.size(); Index++) {
				if (!_stricmp(szBuffer, AuthorizedNames[Index].c_str())) {
					WriteChatf("\ar%s: \ag%s \atis already in your AutoBuff auth list.  \ayNot adding it again.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
					return;
				}
			}
			AuthorizedNames.push_back(szBuffer);
			WriteChatf("\ar%s: \at'%s' added to authorized users list.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atName to add not specified.", PLUGIN_NAME);
	}
	// Block name
	else if (!_stricmp(szTemp, "addblock")) {
		GetArg(szBuffer, szLine, 2);
		if (strlen(szBuffer) > 0) {
			_strlwr_s(szBuffer);
			for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
				if (!_stricmp(szBuffer, BlockedNames[Index].c_str())) {
					WriteChatf("\ar%s: \ag%s \atis already in your AutoBuff block list.  \ayNot adding it again.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
					return;
				}
			}
			BlockedNames.push_back(szBuffer);
			WriteChatf("\ar%s: \at'%s' added to block list.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atName to block not specified.", PLUGIN_NAME);
	}
	// Delete name #
	else if (!_stricmp(szTemp, "del")) {
		GetArg(szBuffer, szLine, 2);
		unsigned int Index = atoi(szBuffer);
		if (Index > 0 && Index < AuthorizedNames.size() + 1) {
			WriteChatf("\ar%s: \atName \ag%d \at(\ar%s\at) deleted.", PLUGIN_NAME, Index, MakeProperCase(AuthorizedNames[Index - 1]));
			AuthorizedNames.erase(AuthorizedNames.begin() + (Index - 1));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atName %d does not exist.", PLUGIN_NAME, Index);
	}
	// Delete block #
	else if (!_stricmp(szTemp, "delblock")) {
		GetArg(szBuffer, szLine, 2);
		unsigned int Index = atoi(szBuffer);
		if (Index > 0 && Index < BlockedNames.size() + 1) {
			WriteChatf("\ar%s: \atBlocked Name \ag%d \at(\ar%s\at) deleted.", PLUGIN_NAME, Index, MakeProperCase(BlockedNames[Index - 1]));
			BlockedNames.erase(BlockedNames.begin() + (Index - 1));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atBlocked Name %d does not exist.", PLUGIN_NAME, Index);
	}
	// Clear all names
	else if (!_stricmp(szTemp, "clear")) {
		AuthorizedNames.clear();
		WriteChatf("\ar%s: \atAuthorized names cleared.", PLUGIN_NAME);
	}
	// Clear all blocked names
	else if (!_stricmp(szTemp, "clearblock")) {
		BlockedNames.clear();
		WriteChatf("\ar%s: \atBlocked names cleared.", PLUGIN_NAME);
	}
	// List all names
	else if (!_stricmp(szTemp, "list")) {
		WriteChatf("\ar%s: \atNames loaded: \ag%d\at.", PLUGIN_NAME, AuthorizedNames.size());
		for (unsigned int Index = 0; Index < AuthorizedNames.size(); Index++) {
			WriteChatf("\at%d\ax:\ay %s", Index + 1, MakeProperCase(AuthorizedNames[Index]));
		}
	}
	// List all blocked names
	else if (!_stricmp(szTemp, "listblock")) {
		WriteChatf("\ar%s: \atBlocked Names loaded: \ag%d\at.", PLUGIN_NAME, BlockedNames.size());
		for (unsigned int Index = 0; Index < BlockedNames.size(); Index++) {
			WriteChatf("\at%d\ax:\ay %s", Index + 1, MakeProperCase(BlockedNames[Index]));
		}
	}
	// Add guild name
	else if (!_stricmp(szTemp, "gadd")) {
		GetArg(szBuffer, szLine, 2);
		if (strlen(szBuffer) > 0) {
			_strlwr_s(szBuffer);
			for (unsigned int Index = 0; Index < AuthorizedGuilds.size(); Index++) {
				if (!_stricmp(szBuffer, AuthorizedGuilds[Index].c_str())) {
					WriteChatf("\ar%s: \ag%s \atis already in your AutoBuff guild auth list.  \ayNot adding it again.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
					return;
				}
			}
			AuthorizedGuilds.push_back(szBuffer);
			WriteChatf("\ar%s: \at'%s' added to authorized guilds list.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atGuild name to add not specified.", PLUGIN_NAME);
	}
	// Block guild name
	else if (!_stricmp(szTemp, "gaddblock")) {
		GetArg(szBuffer, szLine, 2);
		if (strlen(szBuffer) > 0) {
			_strlwr_s(szBuffer);
			for (unsigned int Index = 0; Index < BlockedGuilds.size(); Index++) {
				if (!_stricmp(szBuffer, BlockedGuilds[Index].c_str())) {
					WriteChatf("\ar%s: \ag%s \atis already in your AutoBuff guild block list.  \ayNot adding it again.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
					return;
				}
			}
			BlockedGuilds.push_back(szBuffer);
			WriteChatf("\ar%s: \at'%s' added to guild block list.", PLUGIN_NAME, MakeProperCase(string(szBuffer)));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atGuild name to block not specified.", PLUGIN_NAME);
	}
	// Delete guild #
	else if (!_stricmp(szTemp, "gdel")) {
		GetArg(szBuffer, szLine, 2);
		unsigned int Index = atoi(szBuffer);
		if (Index > 0 && Index < AuthorizedGuilds.size() + 1) {
			WriteChatf("\ar%s: \atGuild \ag%d \at(\ar%s\at) deleted.", PLUGIN_NAME, Index, MakeProperCase(AuthorizedGuilds[Index - 1]));
			AuthorizedGuilds.erase(AuthorizedGuilds.begin() + (Index - 1));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atGuild %d does not exist.", PLUGIN_NAME, Index);
	}
	// Delete guild block #
	else if (!_stricmp(szTemp, "gdelblock")) {
		GetArg(szBuffer, szLine, 2);
		unsigned int Index = atoi(szBuffer);
		if (Index > 0 && Index < BlockedGuilds.size() + 1) {
			WriteChatf("\ar%s: \atBlocked guild \ag%d \at(\ar%s\at) deleted.", PLUGIN_NAME, Index, MakeProperCase(BlockedGuilds[Index - 1]));
			BlockedGuilds.erase(BlockedGuilds.begin() + (Index - 1));
			SaveINI();
		}
		else
			WriteChatf("\ar%s: \atBlocked Guild %d does not exist.", PLUGIN_NAME, Index);
	}
	// Clear all authorized guild names
	else if (!_stricmp(szTemp, "gclear")) {
		AuthorizedGuilds.clear();
		WriteChatf("\ar%s: \atAuthorized guild names cleared.", PLUGIN_NAME);
	}
	// Clear all blocked guild names
	else if (!_stricmp(szTemp, "gclearblock")) {
		BlockedGuilds.clear();
		WriteChatf("\ar%s: \atBlocked guild names cleared.", PLUGIN_NAME);
	}
	// List all guild names
	else if (!_stricmp(szTemp, "glist")) {
		WriteChatf("\ar%s: \atGuilds loaded: \ag%d\at.", PLUGIN_NAME, AuthorizedGuilds.size());
		for (unsigned int Index = 0; Index < AuthorizedGuilds.size(); Index++) {
			WriteChatf("\at%d\ax:\ay %s", Index + 1, MakeProperCase(AuthorizedGuilds[Index]));
		}
	}
	// List all blocked guild names
	else if (!_stricmp(szTemp, "glistblock")) {
		WriteChatf("\ar%s: \atBlocked Guild names loaded: \ag%d\at.", PLUGIN_NAME, BlockedGuilds.size());
		for (unsigned int Index = 0; Index < BlockedGuilds.size(); Index++) {
			WriteChatf("\at%d\ax:\ay %s", Index + 1, MakeProperCase(BlockedGuilds[Index]));
		}
	}
	// No parameter is status
	else if (!strlen(szTemp)) {
		WriteChatf("\ar%s: \atUse Authorized/Blocked Names Lists: %s\ax.", PLUGIN_NAME, bCharControl ? "\agON" : "\arOFF");
		WriteChatf("\ar%s: \atUse Authorized/Blocked Guilds Lists: %s\ax.", PLUGIN_NAME, bGuildControl ? "\agON" : "\arOFF");
		WriteChatf("\ar%s: \atAlways Authorize Own Guild: %s\ax.", PLUGIN_NAME, bAutoBuffGuild ? "\agON" : "\arOFF");
		WriteChatf("\ar%s: \atAlways Authorize Own Raid: %s\ax.", PLUGIN_NAME, bAutoBuffRaid ? "\agON" : "\arOFF");
		WriteChatf("\ar%s: \atAuto Add Own Chars As Authorized Users: %s\ax.", PLUGIN_NAME, bAutoAdd ? "\agON" : "\arOFF");
		WriteChatf("\ar%s: \atAuthorized Names loaded: \ag%d\at.", PLUGIN_NAME, AuthorizedNames.size());
		WriteChatf("\ar%s: \atBlocked Names loaded: \ag%d\at.", PLUGIN_NAME, BlockedNames.size());
		WriteChatf("\ar%s: \atAuthorized Guilds loaded: \ag%d\at.", PLUGIN_NAME, AuthorizedGuilds.size());
		WriteChatf("\ar%s: \atBlocked Guilds loaded: \ag%d\at.", PLUGIN_NAME, BlockedGuilds.size());
		WriteChatf("\ar%s: \agType \ay/abc help \agfor usage.", PLUGIN_NAME);
	}
	// Otherwise, show help
	else {
		WriteChatf("\atUsage\ax: \ag/abc [optional parameter, see below]\n");
		WriteChatf("\ag  Automatically accepts buff requests from specified chars, or guild, or raid.");
		WriteChatf("\ag  Character names and guild names are NOT case sensitive.");
		WriteChatf("\ag  Using \ar/abc\ag with no options will show current status.\n");
		WriteChatf("\at  on \ax= \ayTurns use authorized names list on.");
		WriteChatf("\at  off \ax= \ayTurns use authorized names list off.");
		WriteChatf("\at  gon \ax= \ayTurns use authorized guilds list on.");
		WriteChatf("\at  goff \ax= \ayTurns use authorized guilds list off.");
		WriteChatf("\at  guild \ax= \ayToggles always accepting buff request from your own guild members (if any).");
		WriteChatf("\at  raid \ax= \ayToggles always accepting buff request from your own raid members (if any).");
		WriteChatf("\at  load \ax= \ayLoads options and names from .ini file");
		WriteChatf("\ay             (any current unsaved names will be lost!).");
		WriteChatf("\at  save \ax= \ayUpdates .ini to match current options and names.");
		WriteChatf("\at  auto \ax= \ayToggles automatically adding your own chars to INI (if on, any char you log in will be added to auth list)");
		WriteChatf("\at  add \ax= \ayAdd a new char name (ex: \ar/abc add bubbawar\ay).");
		WriteChatf("\at  del \ax= \ayDelete a char name (ex: \ar/abc del 15\ay deletes name #15).");
		WriteChatf("\at  clear \ax= \ayClears all char names.");
		WriteChatf("\at  list \ax= \ayLists current char names.");
		WriteChatf("\at  addblock \ax= \ayAdd a new char name to your block list (ex: \ar/abc addblock bubbawar\ay).");
		WriteChatf("\at  delblock \ax= \ayDelete a char name from your block list (ex: \ar/abc delblock 15\ay deletes block #15).");
		WriteChatf("\at  clearblock \ax= \ayClears all blocked names.");
		WriteChatf("\at  listblock \ax= \ayLists current blocked names.");
		WriteChatf("\at  gadd \ax= \ayAdd a new guild name (ex: \ar/abc gadd \"rat bastards\"\ay).");
		WriteChatf("\at  gdel \ax= \ayDelete a guild name (ex: \ar/abc gdel 15\ay deletes guild #15).");
		WriteChatf("\at  gclear \ax= \ayClears all guild names.");
		WriteChatf("\at  glist \ax= \ayLists current guild names.");
		WriteChatf("\at  gaddblock \ax= \ayAdd a new guild name to your block list (ex: \ar/abc gaddblock \"rat bastards\"\ay).");
		WriteChatf("\at  gdelblock \ax= \ayDelete a guild name from your block list (ex: \ar/abc gdelblock 15\ay deletes block #15).");
		WriteChatf("\at  gclearblock \ax= \ayClears all blocked guilds.");
		WriteChatf("\at  glistblock \ax= \ayLists current blocked guilds.\n");
	}
}
