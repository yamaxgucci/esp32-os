/*
 * Harbor Quest — maps, dialogs, combat tables (globals; include before funcs).
 *
 * Plot beats follow a shortened Traysia Ch.1 walkthrough (original English).
 */
#ifndef HQ_DATA_H
#define HQ_DATA_H

#define TW 16
#define TH 16
#define ATLAS_W 16
#define ATLAS_N 128
#define ATLAS_BYTES (ATLAS_W * 8 * TW * TH * 2)

#define VIEW_W 640
#define VIEW_H 400
#define SAVE_PATH "h:\\harbor.sav"
#define SAVE_MAGIC 0x48515231
#define SAVE_VER 1
#define AG_O_RDONLY 0
#define AG_O_WRONLY 1
#define AG_O_CREATE 4
#define AG_O_TRUNC 8

#define MAP_TOWN 0
#define MAP_FIELD 1
#define MAP_GRAY 2
#define MAP_CAVE 3
#define MAP_N 4

#define TOWN_W 36
#define TOWN_H 28
#define FIELD_W 48
#define FIELD_H 32
#define GRAY_W 28
#define GRAY_H 22
#define CAVE_W 30
#define CAVE_H 24

#define T_GRASS 0
#define T_GRASS2 1
#define T_DIRT 2
#define T_SAND 3
#define T_WATER 4
#define T_ROAD 5
#define T_WALL 6
#define T_FLOOR 7
#define T_DOOR 8
#define T_TREE 9
#define T_ROCK 10
#define T_BUSH 11
#define T_CARPET 12
#define T_COUNTER 13
#define T_CWALL 14
#define T_CFLOOR 15
#define T_ROOF 16
#define T_WINDOW 17
#define T_BED 18
#define T_CHEST 19
#define T_SIGN 20
#define T_STAIRS 21
#define T_BRIDGE 22
#define T_FENCE 23
#define T_DEEP 24
#define T_PIER 25
#define T_STONE 26
#define T_VOID 27
#define T_FLOWER 28
#define T_SAIL 29
#define T_PATH 30
#define T_TORCH 31

#define SP_HERO0 32
#define SP_LINA 40
#define SP_BAN 42
#define SP_SHOP 44
#define SP_VILL 45
#define SP_ELDER 46
#define SP_INN 47
#define SP_SLIME 48
#define SP_BANDIT 50
#define SP_BAT 52
#define SP_BOSS 54

#define QF_LINA 1
#define QF_SWORD 2
#define QF_BAN_MET 4
#define QF_BAN_JOIN 8
#define QF_CAVE 16
#define QF_DONE 32

#define ST_TITLE 0
#define ST_FIELD 1
#define ST_DIALOG 2
#define ST_BATTLE 3
#define ST_SHOP 4
#define ST_INN 5
#define ST_WIN 6
#define ST_DEAD 7

#define NPC_MAX 12
#define PARTY_MAX 2
#define EN_MAX 2

#define KEY_RGB 16711935

char atlas[ATLAS_BYTES];
char map_town[TOWN_W * TOWN_H];
char map_field[FIELD_W * FIELD_H];
char map_gray[GRAY_W * GRAY_H];
char map_cave[CAVE_W * CAVE_H];

int map_id;
int map_w;
int map_h;
char *map_ptr;

int ox;
int oy;
int px;
int py;
int pdir;
int pframe;
int panim;
int move_cool;
int steps;
int tick;
int state;
int running;
int qflags;
int gold;
int potions;
int has_sword;
int enc_cool;

int hero_hp;
int hero_mhp;
int hero_atk;
int hero_def;
int hero_lvl;
int hero_exp;

int ban_hp;
int ban_mhp;
int ban_atk;
int ban_def;
int ban_in;

int npc_n;
int npc_x[NPC_MAX];
int npc_y[NPC_MAX];
int npc_hx[NPC_MAX];
int npc_hy[NPC_MAX];
int npc_rad[NPC_MAX];
int npc_cool[NPC_MAX];
int npc_sp[NPC_MAX];
int npc_dlg[NPC_MAX];
int npc_frame[NPC_MAX];

int dlg_i;
int dlg_n;
char *dlg0;
char *dlg1;
char *dlg2;
char *dlg3;
char *dlg4;
char *dlg5;
char *dlg6;
char *dlg7;

int bat_n;
int bat_hp[EN_MAX];
int bat_mhp[EN_MAX];
int bat_atk[EN_MAX];
int bat_def[EN_MAX];
int bat_sp[EN_MAX];
int bat_exp[EN_MAX];
int bat_gold[EN_MAX];
int bat_boss;
int bat_sel;
char *bat_a;
char *bat_b;
int bat_turn;
int atlas_ok;

char *name_roy;
char *name_lina;
char *name_ban;
char numbuf[12];
char *mw_map;
int mw_w;
char savbuf[128];
int sav_off;
int save_ok;
int need_save;

#endif
