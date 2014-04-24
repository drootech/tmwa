#include "map.hpp"
//    map.cpp - Core of the map server.
//
//    Copyright © ????-2004 Athena Dev Teams
//    Copyright © 2004-2011 The Mana World Development Team
//    Copyright © 2011-2014 Ben Longbons <b.r.longbons@gmail.com>
//    Copyright © 2013 Freeyorp
//
//    This file is part of The Mana World (Athena server)
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <sys/time.h>
#include <sys/wait.h>

#include <netdb.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>

#include "../compat/nullpo.hpp"
#include "../compat/fun.hpp"

#include "../ints/udl.hpp"

#include "../strings/astring.hpp"
#include "../strings/zstring.hpp"
#include "../strings/xstring.hpp"
#include "../strings/vstring.hpp"
#include "../strings/literal.hpp"

#include "../generic/db.hpp"
#include "../generic/random2.hpp"

#include "../io/cxxstdio.hpp"
#include "../io/read.hpp"
#include "../io/tty.hpp"
#include "../io/write.hpp"

#include "../mmo/config_parse.hpp"
#include "../mmo/core.hpp"
#include "../mmo/extract.hpp"
#include "../mmo/socket.hpp"
#include "../mmo/timer.hpp"
#include "../mmo/utils.hpp"
#include "../mmo/version.hpp"

#include "atcommand.hpp"
#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "grfio.hpp"
#include "itemdb.hpp"
#include "magic.hpp"
#include "magic-interpreter.hpp" // for is_spell inline body
#include "magic-v2.hpp"
#include "mob.hpp"
#include "npc.hpp"
#include "party.hpp"
#include "pc.hpp"
#include "script.hpp"
#include "skill.hpp"
#include "storage.hpp"
#include "trade.hpp"

#include "../poison.hpp"

DMap<BlockId, dumb_ptr<block_list>> id_db;

UPMap<MapName, map_abstract> maps_db;

static
DMap<CharName, dumb_ptr<map_session_data>> nick_db;

struct charid2nick
{
    CharName nick;
    int req_id;
};

static
Map<CharId, struct charid2nick> charid_db;

static
int users = 0;
static
Array<dumb_ptr<block_list>, unwrap<BlockId>(MAX_FLOORITEM)> object;
static
BlockId first_free_object_id = BlockId();

interval_t autosave_time = DEFAULT_AUTOSAVE_INTERVAL;
int save_settings = 0xFFFF;

AString motd_txt = "conf/motd.txt"_s;

CharName wisp_server_name = stringish<CharName>("Server"_s);   // can be modified in char-server configuration file

static
void map_delmap(MapName mapname);

void SessionDeleter::operator()(SessionData *sd)
{
    really_delete1 static_cast<map_session_data *>(sd);
}

VString<49> convert_for_printf(NpcEvent ev)
{
    return STRNPRINTF(50, "%s::%s"_fmt, ev.npc, ev.label);
}
bool extract(XString str, NpcEvent *ev)
{
    XString mid;
    return extract(str, record<':'>(&ev->npc, &mid, &ev->label)) && !mid;
}

/*==========================================
 * 全map鯖総計での接続数設定
 * (char鯖から送られてくる)
 *------------------------------------------
 */
void map_setusers(int n)
{
    users = n;
}

/*==========================================
 * 全map鯖総計での接続数取得 (/wへの応答用)
 *------------------------------------------
 */
int map_getusers(void)
{
    return users;
}

static
int block_free_lock = 0;
static
std::vector<dumb_ptr<block_list>> block_free;

void MapBlockLock::freeblock(dumb_ptr<block_list> bl)
{
    if (block_free_lock == 0)
        bl.delete_();
    else
        block_free.push_back(bl);
}

MapBlockLock::MapBlockLock()
{
    ++block_free_lock;
}

MapBlockLock::~MapBlockLock()
{
    assert (block_free_lock > 0);
    if ((--block_free_lock) == 0)
    {
        for (dumb_ptr<block_list> bl : block_free)
            bl.delete_();
        block_free.clear();
    }
}

/// This is a dummy entry that is shared by all the linked lists,
/// so that any entry can unlink itself without worrying about
/// whether it was the the head of the list.
static
struct block_list bl_head;

/*==========================================
 * map[]のblock_listに追加
 * mobは数が多いので別リスト
 *
 * 既にlink済みかの確認が無い。危険かも
 *------------------------------------------
 */
int map_addblock(dumb_ptr<block_list> bl)
{
    nullpo_ret(bl);

    if (bl->bl_prev)
    {
        if (battle_config.error_log)
            PRINTF("map_addblock error : bl->bl_prev!=NULL\n"_fmt);
        return 0;
    }

    map_local *m = bl->bl_m;
    int x = bl->bl_x;
    int y = bl->bl_y;
    if (!m ||
        x < 0 || x >= m->xs || y < 0 || y >= m->ys)
        return 1;

    if (bl->bl_type == BL::MOB)
    {
        bl->bl_next = m->blocks.ref(x / BLOCK_SIZE, y / BLOCK_SIZE).mobs_only;
        bl->bl_prev = dumb_ptr<block_list>(&bl_head);
        if (bl->bl_next)
            bl->bl_next->bl_prev = bl;
        m->blocks.ref(x / BLOCK_SIZE, y / BLOCK_SIZE).mobs_only = bl;
    }
    else
    {
        bl->bl_next = m->blocks.ref(x / BLOCK_SIZE, y / BLOCK_SIZE).normal;
        bl->bl_prev = dumb_ptr<block_list>(&bl_head);
        if (bl->bl_next)
            bl->bl_next->bl_prev = bl;
        m->blocks.ref(x / BLOCK_SIZE, y / BLOCK_SIZE).normal = bl;
        if (bl->bl_type == BL::PC)
            m->users++;
    }

    return 0;
}

/*==========================================
 * map[]のblock_listから外す
 * prevがNULLの場合listに繋がってない
 *------------------------------------------
 */
int map_delblock(dumb_ptr<block_list> bl)
{
    nullpo_ret(bl);

    // 既にblocklistから抜けている
    if (!bl->bl_prev)
    {
        if (bl->bl_next)
        {
            // prevがNULLでnextがNULLでないのは有ってはならない
            if (battle_config.error_log)
                PRINTF("map_delblock error : bl->bl_next!=NULL\n"_fmt);
        }
        return 0;
    }

    if (bl->bl_type == BL::PC)
        bl->bl_m->users--;

    if (bl->bl_next)
        bl->bl_next->bl_prev = bl->bl_prev;
    if (bl->bl_prev == dumb_ptr<block_list>(&bl_head))
    {
        // リストの頭なので、map[]のblock_listを更新する
        if (bl->bl_type == BL::MOB)
        {
            bl->bl_m->blocks.ref(bl->bl_x / BLOCK_SIZE, bl->bl_y / BLOCK_SIZE).mobs_only = bl->bl_next;
        }
        else
        {
            bl->bl_m->blocks.ref(bl->bl_x / BLOCK_SIZE, bl->bl_y / BLOCK_SIZE).normal = bl->bl_next;
        }
    }
    else
    {
        bl->bl_prev->bl_next = bl->bl_next;
    }
    bl->bl_next = NULL;
    bl->bl_prev = NULL;

    return 0;
}

/*==========================================
 * セル上のPCとMOBの数を数える (グランドクロス用)
 *------------------------------------------
 */
int map_count_oncell(map_local *m, int x, int y)
{
    int bx, by;
    dumb_ptr<block_list> bl = NULL;
    int count = 0;

    if (x < 0 || y < 0 || (x >= m->xs) || (y >= m->ys))
        return 1;
    bx = x / BLOCK_SIZE;
    by = y / BLOCK_SIZE;

    bl = m->blocks.ref(bx, by).normal;
    for (; bl; bl = bl->bl_next)
    {
        if (bl->bl_x == x && bl->bl_y == y && bl->bl_type == BL::PC)
            count++;
    }
    bl = m->blocks.ref(bx, by).mobs_only;
    for (; bl; bl = bl->bl_next)
    {
        if (bl->bl_x == x && bl->bl_y == y)
            count++;
    }
    if (!count)
        count = 1;
    return count;
}

/*==========================================
 * map m (x0,y0)-(x1,y1)内の全objに対して
 * funcを呼ぶ
 * type!=0 ならその種類のみ
 *------------------------------------------
 */
void map_foreachinarea(std::function<void(dumb_ptr<block_list>)> func,
        map_local *m,
        int x0, int y0, int x1, int y1,
        BL type)
{
    std::vector<dumb_ptr<block_list>> bl_list;

    if (!m)
        return;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 >= m->xs)
        x1 = m->xs - 1;
    if (y1 >= m->ys)
        y1 = m->ys - 1;
    if (type == BL::NUL || type != BL::MOB)
        for (int by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++)
        {
            for (int bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++)
            {
                dumb_ptr<block_list> bl = m->blocks.ref(bx, by).normal;
                for (; bl; bl = bl->bl_next)
                {
                    if (type != BL::NUL && bl->bl_type != type)
                        continue;
                    if (bl->bl_x >= x0 && bl->bl_x <= x1
                        && bl->bl_y >= y0 && bl->bl_y <= y1)
                        bl_list.push_back(bl);
                }
            }
        }
    if (type == BL::NUL || type == BL::MOB)
        for (int by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++)
        {
            for (int bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++)
            {
                dumb_ptr<block_list> bl = m->blocks.ref(bx, by).mobs_only;
                for (; bl; bl = bl->bl_next)
                {
                    if (bl->bl_x >= x0 && bl->bl_x <= x1
                        && bl->bl_y >= y0 && bl->bl_y <= y1)
                        bl_list.push_back(bl);
                }
            }
        }

    MapBlockLock lock;

    for (dumb_ptr<block_list> bl : bl_list)
        if (bl->bl_prev)
            func(bl);
}

/*==========================================
 * 矩形(x0,y0)-(x1,y1)が(dx,dy)移動した時の
 * 領域外になる領域(矩形かL字形)内のobjに
 * 対してfuncを呼ぶ
 *
 * dx,dyは-1,0,1のみとする（どんな値でもいいっぽい？）
 *------------------------------------------
 */
void map_foreachinmovearea(std::function<void(dumb_ptr<block_list>)> func,
        map_local *m,
        int x0, int y0, int x1, int y1,
        int dx, int dy,
        BL type)
{
    std::vector<dumb_ptr<block_list>> bl_list;
    // Note: the x0, y0, x1, y1 are bl.bl_x, bl.bl_y ± AREA_SIZE,
    // but only a small subset actually needs to be done.
    if (dx == 0 || dy == 0)
    {
        assert (dx || dy);
        // for aligned movement, only needs to check a rectangular area
        if (dx == 0)
        {
            if (dy < 0)
                y0 = y1 + dy + 1;
            else
                y1 = y0 + dy - 1;
        }
        else if (dy == 0)
        {
            if (dx < 0)
                x0 = x1 + dx + 1;
            else
                x1 = x0 + dx - 1;
        }
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 >= m->xs)
            x1 = m->xs - 1;
        if (y1 >= m->ys)
            y1 = m->ys - 1;
        for (int by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++)
        {
            for (int bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++)
            {
                dumb_ptr<block_list> bl = m->blocks.ref(bx, by).normal;
                for (; bl; bl = bl->bl_next)
                {
                    if (type != BL::NUL && bl->bl_type != type)
                        continue;
                    if (bl->bl_x >= x0 && bl->bl_x <= x1
                        && bl->bl_y >= y0 && bl->bl_y <= y1)
                        bl_list.push_back(bl);
                }
                bl = m->blocks.ref(bx, by).mobs_only;
                for (; bl; bl = bl->bl_next)
                {
                    if (type != BL::NUL && bl->bl_type != type)
                        continue;
                    if (bl->bl_x >= x0 && bl->bl_x <= x1
                        && bl->bl_y >= y0 && bl->bl_y <= y1)
                        bl_list.push_back(bl);
                }
            }
        }
    }
    else
    {
        // L字領域の場合

        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 >= m->xs)
            x1 = m->xs - 1;
        if (y1 >= m->ys)
            y1 = m->ys - 1;
        for (int by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++)
        {
            for (int bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++)
            {
                dumb_ptr<block_list> bl = m->blocks.ref(bx, by).normal;
                for (; bl; bl = bl->bl_next)
                {
                    if (type != BL::NUL && bl->bl_type != type)
                        continue;
                    if (!(bl->bl_x >= x0 && bl->bl_x <= x1
                            && bl->bl_y >= y0 && bl->bl_y <= y1))
                        continue;
                    if ((dx > 0 && bl->bl_x < x0 + dx)
                        || (dx < 0 && bl->bl_x > x1 + dx)
                        || (dy > 0 && bl->bl_y < y0 + dy)
                        || (dy < 0 && bl->bl_y > y1 + dy))
                        bl_list.push_back(bl);
                }
                bl = m->blocks.ref(bx, by).mobs_only;
                for (; bl; bl = bl->bl_next)
                {
                    if (type != BL::NUL && bl->bl_type != type)
                        continue;
                    if (!(bl->bl_x >= x0 && bl->bl_x <= x1
                             && bl->bl_y >= y0 && bl->bl_y <= y1))
                        continue;
                    if ((dx > 0 && bl->bl_x < x0 + dx)
                        || (dx < 0 && bl->bl_x > x1 + dx)
                        || (dy > 0 && bl->bl_y < y0 + dy)
                        || (dy < 0 && bl->bl_y > y1 + dy))
                        bl_list.push_back(bl);
                }
            }
        }

    }

    MapBlockLock lock;

    for (dumb_ptr<block_list> bl : bl_list)
        if (bl->bl_prev)
            func(bl);
}

// -- moonsoul  (added map_foreachincell which is a rework of map_foreachinarea but
//           which only checks the exact single x/y passed to it rather than an
//           area radius - may be more useful in some instances)
//
void map_foreachincell(std::function<void(dumb_ptr<block_list>)> func,
        map_local *m,
        int x, int y,
        BL type)
{
    std::vector<dumb_ptr<block_list>> bl_list;
    int by = y / BLOCK_SIZE;
    int bx = x / BLOCK_SIZE;

    if (type == BL::NUL || type != BL::MOB)
    {
        dumb_ptr<block_list> bl = m->blocks.ref(bx, by).normal;
        for (; bl; bl = bl->bl_next)
        {
            if (type != BL::NUL && bl->bl_type != type)
                continue;
            if (bl->bl_x == x && bl->bl_y == y)
                bl_list.push_back(bl);
        }
    }

    if (type == BL::NUL || type == BL::MOB)
    {
        dumb_ptr<block_list> bl = m->blocks.ref(bx, by).mobs_only;
        for (; bl; bl = bl->bl_next)
        {
            if (bl->bl_x == x && bl->bl_y == y)
                bl_list.push_back(bl);
        }
    }

    MapBlockLock lock;

    for (dumb_ptr<block_list> bl : bl_list)
        if (bl->bl_prev)
            func(bl);
}

/*==========================================
 * 床アイテムやエフェクト用の一時obj割り当て
 * object[]への保存とid_db登録まで
 *
 * bl->bl_idもこの中で設定して問題無い?
 *------------------------------------------
 */
BlockId map_addobject(dumb_ptr<block_list> bl)
{
    BlockId i;
    if (!bl)
    {
        PRINTF("map_addobject nullpo?\n"_fmt);
        return BlockId();
    }
    if (first_free_object_id < wrap<BlockId>(2) || first_free_object_id == MAX_FLOORITEM)
        first_free_object_id = wrap<BlockId>(2);
    for (i = first_free_object_id; i < MAX_FLOORITEM; i = next(i))
        if (!object[i._value])
            break;
    if (i == MAX_FLOORITEM)
    {
        if (battle_config.error_log)
            PRINTF("no free object id\n"_fmt);
        return BlockId();
    }
    first_free_object_id = i;
    object[i._value] = bl;
    id_db.put(i, bl);
    return i;
}

/*==========================================
 * 一時objectの解放
 *      map_delobjectのfreeしないバージョン
 *------------------------------------------
 */
void map_delobjectnofree(BlockId id, BL type)
{
    assert (id < MAX_FLOORITEM);
    if (!object[id._value])
        return;

    if (object[id._value]->bl_type != type)
    {
        FPRINTF(stderr, "Incorrect type: expected %d, got %d\n"_fmt,
                type,
                object[id._value]->bl_type);
        abort();
    }

    map_delblock(object[id._value]);
    id_db.put(id, dumb_ptr<block_list>());
    object[id._value] = nullptr;

    if (id < first_free_object_id)
        first_free_object_id = id;
}

/*==========================================
 * 一時objectの解放
 * block_listからの削除、id_dbからの削除
 * object dataのfree、object[]へのNULL代入
 *
 * addとの対称性が無いのが気になる
 *------------------------------------------
 */
void map_delobject(BlockId id, BL type)
{
    assert (id < MAX_FLOORITEM);
    dumb_ptr<block_list> obj = object[id._value];

    if (obj == NULL)
        return;

    map_delobjectnofree(id, type);
    if (obj->bl_type == BL::PC)     // [Fate] Not sure where else to put this... I'm not sure where delobject for PCs is called from
        pc_cleanup(obj->is_player());

    MapBlockLock::freeblock(obj);
}

/*==========================================
 * 全一時obj相手にfuncを呼ぶ
 *
 *------------------------------------------
 */
void map_foreachobject(std::function<void(dumb_ptr<block_list>)> func,
        BL type)
{
    std::vector<dumb_ptr<block_list>> bl_list;
    for (BlockId i = wrap<BlockId>(2); i < MAX_FLOORITEM; i = next(i))
    {
        if (!object[i._value])
            continue;
        {
            if (type != BL::NUL && object[i._value]->bl_type != type)
                continue;
            bl_list.push_back(object[i._value]);
        }
    }

    MapBlockLock lock;

    for (dumb_ptr<block_list> bl : bl_list)
    {
        // TODO figure out if the second branch can happen
        // bl_prev is non-null for all that are on a map (see bl_head)
        // bl_next is only meaningful for objects that are on a map
        if (bl->bl_prev || bl->bl_next)
            func(bl);
    }
}

/*==========================================
 * 床アイテムを消す
 *
 * data==0の時はtimerで消えた時
 * data!=0の時は拾う等で消えた時として動作
 *
 * 後者は、map_clearflooritem(id)へ
 * map.h内で#defineしてある
 *------------------------------------------
 */
void map_clearflooritem_timer(TimerData *tid, tick_t, BlockId id)
{
    assert (id < MAX_FLOORITEM);
    dumb_ptr<block_list> obj = object[id._value];
    assert (obj && obj->bl_type == BL::ITEM);
    dumb_ptr<flooritem_data> fitem = obj->is_item();
    if (!tid)
        fitem->cleartimer.cancel();
    clif_clearflooritem(fitem, 0);
    map_delobject(fitem->bl_id, BL::ITEM);
}

std::pair<uint16_t, uint16_t> map_randfreecell(map_local *m,
        uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    for (int itr : random_::iterator(w * h))
    {
        int dx = itr % w;
        int dy = itr / w;
        if (!bool(read_gatp(m, x + dx, y + dy) & MapCell::UNWALKABLE))
            return {static_cast<uint16_t>(x + dx), static_cast<uint16_t>(y + dy)};
    }
    return {0_u16, 0_u16};
}

/// Return a randomly selected passable cell within a given range.
static
std::pair<uint16_t, uint16_t> map_searchrandfreecell(map_local *m, int x, int y, int range)
{
    int whole_range = 2 * range + 1;
    return map_randfreecell(m, x - range, y - range, whole_range, whole_range);
}

/*==========================================
 * (m,x,y)を中心に3x3以内に床アイテム設置
 *
 * item_dataはamount以外をcopyする
 *------------------------------------------
 */
BlockId map_addflooritem_any(struct item *item_data, int amount,
        map_local *m, int x, int y,
        dumb_ptr<map_session_data> *owners, interval_t *owner_protection,
        interval_t lifetime, int dispersal)
{
    dumb_ptr<flooritem_data> fitem = NULL;

    nullpo_retr(BlockId(), item_data);
    auto xy = map_searchrandfreecell(m, x, y, dispersal);
    if (xy.first == 0 && xy.second == 0)
        return BlockId();

    fitem.new_();
    fitem->bl_type = BL::ITEM;
    fitem->bl_prev = fitem->bl_next = NULL;
    fitem->bl_m = m;
    fitem->bl_x = xy.first;
    fitem->bl_y = xy.second;
    fitem->first_get_id = BlockId();
    fitem->first_get_tick = tick_t();
    fitem->second_get_id = BlockId();
    fitem->second_get_tick = tick_t();
    fitem->third_get_id = BlockId();
    fitem->third_get_tick = tick_t();

    fitem->bl_id = map_addobject(fitem);
    if (!fitem->bl_id)
    {
        fitem.delete_();
        return BlockId();
    }

    tick_t tick = gettick();

    if (owners[0])
        fitem->first_get_id = owners[0]->bl_id;
    fitem->first_get_tick = tick + owner_protection[0];

    if (owners[1])
        fitem->second_get_id = owners[1]->bl_id;
    fitem->second_get_tick = tick + owner_protection[1];

    if (owners[2])
        fitem->third_get_id = owners[2]->bl_id;
    fitem->third_get_tick = tick + owner_protection[2];

    fitem->item_data = *item_data;
    fitem->item_data.amount = amount;
    // TODO - talk to 4144 about maybe removing this.
    // It has no effect on the server itself, it is visual only.
    // If it is desirable to prevent items from visibly stacking
    // on the ground, that can be done with client-side randomness.
    // Currently, it yields the numbers {3 6 9 12}.
    fitem->subx = random_::in(1, 4) * 3;
    fitem->suby = random_::in(1, 4) * 3;
    fitem->cleartimer = Timer(gettick() + lifetime,
            std::bind(map_clearflooritem_timer, ph::_1, ph::_2,
                fitem->bl_id));

    map_addblock(fitem);
    clif_dropflooritem(fitem);

    return fitem->bl_id;
}

BlockId map_addflooritem(struct item *item_data, int amount,
        map_local *m, int x, int y,
        dumb_ptr<map_session_data> first_sd,
        dumb_ptr<map_session_data> second_sd,
        dumb_ptr<map_session_data> third_sd)
{
    dumb_ptr<map_session_data> owners[3] = { first_sd, second_sd, third_sd };
    interval_t owner_protection[3];

    {
        owner_protection[0] = static_cast<interval_t>(battle_config.item_first_get_time);
        owner_protection[1] = owner_protection[0] + static_cast<interval_t>(battle_config.item_second_get_time);
        owner_protection[2] = owner_protection[1] + static_cast<interval_t>(battle_config.item_third_get_time);
    }

    return map_addflooritem_any(item_data, amount, m, x, y,
            owners, owner_protection,
            static_cast<interval_t>(battle_config.flooritem_lifetime), 1);
}

/*==========================================
 * charid_dbへ追加(返信待ちがあれば返信)
 *------------------------------------------
 */
void map_addchariddb(CharId charid, CharName name)
{
    struct charid2nick *p = charid_db.search(charid);
    if (p == NULL)
        p = charid_db.init(charid);

    p->nick = name;
    p->req_id = 0;
}

/*==========================================
 * id_dbへblを追加
 *------------------------------------------
 */
void map_addiddb(dumb_ptr<block_list> bl)
{
    nullpo_retv(bl);

    id_db.put(bl->bl_id, bl);
}

/*==========================================
 * id_dbからblを削除
 *------------------------------------------
 */
void map_deliddb(dumb_ptr<block_list> bl)
{
    nullpo_retv(bl);

    id_db.put(bl->bl_id, nullptr);
}

/*==========================================
 * nick_dbへsdを追加
 *------------------------------------------
 */
void map_addnickdb(dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    nick_db.put(sd->status_key.name, sd);
}

/*==========================================
 * PCのquit処理 map.c内分
 *
 * quit処理の主体が違うような気もしてきた
 *------------------------------------------
 */
void map_quit(dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    if (sd->trade_partner)      // 取引を中断する
        trade_tradecancel(sd);

    if (sd->party_invite)   // パーティ勧誘を拒否する
        party_reply_invite(sd, sd->party_invite_account, 0);

    party_send_logout(sd);     // パーティのログアウトメッセージ送信

    pc_cleareventtimer(sd);    // イベントタイマを破棄する

    skill_castcancel(sd, 0);  // 詠唱を中断する
    skill_stop_dancing(sd, 1);    // ダンス/演奏中断

    skill_status_change_clear(sd, 1); // ステータス異常を解除する
    pc_stop_walking(sd, 0);
    pc_stopattack(sd);
    pc_delinvincibletimer(sd);

    pc_calcstatus(sd, 4);

    clif_clearchar(sd, BeingRemoveWhy::QUIT);

    if (pc_isdead(sd))
        pc_setrestartvalue(sd, 2);
    pc_makesavestatus(sd);
    //クローンスキルで覚えたスキルは消す

    //The storage closing routines will save the char if needed. [Skotlex]
    if (!sd->state.storage_open)
        chrif_save(sd);
    else if (sd->state.storage_open)
        storage_storage_quit(sd);

    sd->npc_stackbuf.clear();

    map_delblock(sd);

    id_db.put(sd->bl_id, nullptr);
    nick_db.put(sd->status_key.name, nullptr);
    charid_db.erase(sd->status_key.char_id);
}

/*==========================================
 * id番号のPCを探す。居なければNULL
 *------------------------------------------
 */
dumb_ptr<map_session_data> map_id2sd(BlockId id)
{
    // This is bogus.
    // However, there might be differences for de-auth'ed accounts.
// remove search from db, because:
// 1 - all players, npc, items and mob are in this db (to search, it's not speed, and search in session is more sure)
// 2 - DB seems not always correct. Sometimes, when a player disconnects, its id (account value) is not removed and structure
//     point to a memory area that is not more a session_data and value are incorrect (or out of available memory) -> crash
// replaced by searching in all session.
// by searching in session, we are sure that fd, session, and account exist.
/*
        dumb_ptr<block_list> bl;

        bl=numdb_search(id_db,id);
        if (bl && bl->bl_type==BL::PC)
                return (struct map_session_data*)bl;
        return NULL;
*/
    for (io::FD i : iter_fds())
    {
        Session *s = get_session(i);
        if (!s)
            continue;
        if (s->session_data)
        {
            map_session_data *sd = static_cast<map_session_data *>(s->session_data.get());
            if (sd->bl_id == id)
                return dumb_ptr<map_session_data>(sd);
        }
    }

    return NULL;
}

/*==========================================
 * char_id番号の名前を探す
 *------------------------------------------
 */
CharName map_charid2nick(CharId id)
{
    struct charid2nick *p = charid_db.search(id);

    if (p == NULL)
        return CharName();
    if (p->req_id != 0)
        return CharName();
    return p->nick;
}

/*========================================*/
/* [Fate] Operations to iterate over active map sessions */

static
dumb_ptr<map_session_data> map_get_session(io::FD i)
{
    {
        Session *s = get_session(i);
        if (!s)
            return nullptr;
        map_session_data *d = static_cast<map_session_data *>(s->session_data.get());
        if (d && d->state.auth)
            return dumb_ptr<map_session_data>(d);
    }

    return NULL;
}

static
dumb_ptr<map_session_data> map_get_session_forward(int start)
{
    for (int i = start; i < get_fd_max(); i++)
    {
        dumb_ptr<map_session_data> d = map_get_session(io::FD::cast_dammit(i));
        if (d)
            return d;
    }

    return NULL;
}

static
dumb_ptr<map_session_data> map_get_session_backward(int start)
{
    for (int i = start; i >= 0; i--)
    {
        dumb_ptr<map_session_data> d = map_get_session(io::FD::cast_dammit(i));
        if (d)
            return d;
    }

    return NULL;
}

dumb_ptr<map_session_data> map_get_first_session(void)
{
    return map_get_session_forward(0);
}

dumb_ptr<map_session_data> map_get_next_session(dumb_ptr<map_session_data> d)
{
    return map_get_session_forward(d->sess->fd.uncast_dammit() + 1);
}

dumb_ptr<map_session_data> map_get_last_session(void)
{
    return map_get_session_backward(get_fd_max());
}

dumb_ptr<map_session_data> map_get_prev_session(dumb_ptr<map_session_data> d)
{
    return map_get_session_backward(d->sess->fd.uncast_dammit() - 1);
}

/*==========================================
 * Search session data from a nick name
 * (without sensitive case if necessary)
 * return map_session_data pointer or NULL
 *------------------------------------------
 */
dumb_ptr<map_session_data> map_nick2sd(CharName nick)
{
    for (io::FD i : iter_fds())
    {
        Session *s = get_session(i);
        if (!s)
            continue;
        map_session_data *pl_sd = static_cast<map_session_data *>(s->session_data.get());
        if (pl_sd && pl_sd->state.auth)
        {
            {
                if (pl_sd->status_key.name == nick)
                    return dumb_ptr<map_session_data>(pl_sd);
            }
        }
    }
    return NULL;
}

/*==========================================
 * id番号の物を探す
 * 一時objectの場合は配列を引くのみ
 *------------------------------------------
 */
dumb_ptr<block_list> map_id2bl(BlockId id)
{
    dumb_ptr<block_list> bl = NULL;
    if (id < MAX_FLOORITEM)
        bl = object[id._value];
    else
        bl = id_db.get(id);

    return bl;
}

/*==========================================
 * map.npcへ追加 (warp等の領域持ちのみ)
 *------------------------------------------
 */
int map_addnpc(map_local *m, dumb_ptr<npc_data> nd)
{
    int i;
    if (!m)
        return -1;
    for (i = 0; i < m->npc_num && i < MAX_NPC_PER_MAP; i++)
        if (m->npc[i] == NULL)
            break;
    if (i == MAX_NPC_PER_MAP)
    {
        if (battle_config.error_log)
            PRINTF("too many NPCs in one map %s\n"_fmt, m->name_);
        return -1;
    }
    if (i == m->npc_num)
    {
        m->npc_num++;
    }

    nullpo_ret(nd);

    m->npc[i] = nd;
    nd->n = i;
    id_db.put(nd->bl_id, nd);

    return i;
}

static
void map_removenpc(void)
{
    int n = 0;

    for (auto& mitp : maps_db)
    {
        if (!mitp.second->gat)
            continue;
        map_local *m = static_cast<map_local *>(mitp.second.get());
        for (int i = 0; i < m->npc_num && i < MAX_NPC_PER_MAP; i++)
        {
            if (m->npc[i] != NULL)
            {
                clif_clearchar(m->npc[i], BeingRemoveWhy::QUIT);
                map_delblock(m->npc[i]);
                id_db.put(m->npc[i]->bl_id, nullptr);
                if (m->npc[i]->npc_subtype == NpcSubtype::SCRIPT)
                {
//                    free(m->npc[i]->u.scr.script);
//                    free(m->npc[i]->u.scr.label_list);
                }
                m->npc[i].delete_();
                n++;
            }
        }
    }
    PRINTF("%d NPCs removed.\n"_fmt, n);
}

/*==========================================
 * map名からmap番号へ変換
 *------------------------------------------
 */
map_local *map_mapname2mapid(MapName name)
{
    map_abstract *md = maps_db.get(name);
    if (md == NULL || md->gat == NULL)
        return nullptr;
    return static_cast<map_local *>(md);
}

/*==========================================
 * 他鯖map名からip,port変換
 *------------------------------------------
 */
int map_mapname2ipport(MapName name, IP4Address *ip, int *port)
{
    map_abstract *md = maps_db.get(name);
    if (md == NULL || md->gat)
        return -1;
    map_remote *mdos = static_cast<map_remote *>(md);
    *ip = mdos->ip;
    *port = mdos->port;
    return 0;
}

/// Check compatibility of directions.
/// Directions are compatible if they are at most 45° apart.
///
/// @return false if compatible, true if incompatible.
bool map_check_dir(const DIR s_dir, const DIR t_dir)
{
    if (s_dir == t_dir)
        return false;

    const uint8_t sdir = static_cast<uint8_t>(s_dir);
    const uint8_t tdir = static_cast<uint8_t>(t_dir);
    if ((sdir + 1) % 8 == tdir)
        return false;
    if (sdir == (tdir + 1) % 8)
        return false;

    return true;
}

/*==========================================
 * 彼我の方向を計算
 *------------------------------------------
 */
DIR map_calc_dir(dumb_ptr<block_list> src, int x, int y)
{
    DIR dir = DIR::S;
    int dx, dy;

    nullpo_retr(DIR::S, src);

    dx = x - src->bl_x;
    dy = y - src->bl_y;
    if (dx == 0 && dy == 0)
    {
        dir = DIR::S;
    }
    else if (dx >= 0 && dy >= 0)
    {
        dir = DIR::SE;
        if (dx * 3 - 1 < dy)
            dir = DIR::S;
        if (dx > dy * 3)
            dir = DIR::E;
    }
    else if (dx >= 0 && dy <= 0)
    {
        dir = DIR::NE;
        if (dx * 3 - 1 < -dy)
            dir = DIR::N;
        if (dx > -dy * 3)
            dir = DIR::E;
    }
    else if (dx <= 0 && dy <= 0)
    {
        dir = DIR::NW;
        if (dx * 3 + 1 > dy)
            dir = DIR::N;
        if (dx < dy * 3)
            dir = DIR::W;
    }
    else
    {
        dir = DIR::SW;
        if (-dx * 3 - 1 < dy)
            dir = DIR::S;
        if (-dx > dy * 3)
            dir = DIR::W;
    }
    return dir;
}

// gat系
/*==========================================
 * (m,x,y)の状態を調べる
 *------------------------------------------
 */
MapCell map_getcell(map_local *m, int x, int y)
{
    if (x < 0 || x >= m->xs - 1 || y < 0 || y >= m->ys - 1)
        return MapCell::UNWALKABLE;
    return m->gat[x + y * m->xs];
}

/*==========================================
 * (m,x,y)の状態をtにする
 *------------------------------------------
 */
void map_setcell(map_local *m, int x, int y, MapCell t)
{
    if (x < 0 || x >= m->xs || y < 0 || y >= m->ys)
        return;
    m->gat[x + y * m->xs] = t;
}

/*==========================================
 * 他鯖管理のマップをdbに追加
 *------------------------------------------
 */
int map_setipport(MapName name, IP4Address ip, int port)
{
    map_abstract *md = maps_db.get(name);
    if (md == NULL)
    {
        // not exist -> add new data
        auto mdos = make_unique<map_remote>();
        mdos->name_ = name;
        mdos->gat = NULL;
        mdos->ip = ip;
        mdos->port = port;
        maps_db.put(mdos->name_, std::move(mdos));
    }
    else
    {
        if (md->gat)
        {
            // local -> check data
            if (ip != clif_getip() || port != clif_getport())
            {
                PRINTF("from char server : %s -> %s:%d\n"_fmt,
                        name, ip, port);
                return 1;
            }
        }
        else
        {
            // update
            map_remote *mdos = static_cast<map_remote *>(md);
            mdos->ip = ip;
            mdos->port = port;
        }
    }
    return 0;
}

/*==========================================
 * マップ1枚読み込み
 *------------------------------------------
 */
static
bool map_readmap(map_local *m, size_t num, MapName fn)
{
    // read & convert fn
    std::vector<uint8_t> gat_v = grfio_reads(fn);
    if (gat_v.empty())
        return false;
    size_t s = gat_v.size() - 4;

    int xs = m->xs = gat_v[0] | gat_v[1] << 8;
    int ys = m->ys = gat_v[2] | gat_v[3] << 8;
    PRINTF("\rLoading Maps [%zu/%zu]: %-30s  (%i, %i)"_fmt,
            num, maps_db.size(),
            fn, xs, ys);
    fflush(stdout);

    assert (s == xs * ys);
    m->gat = make_unique<MapCell[]>(s);

    m->npc_num = 0;
    m->users = 0;
    really_memzero_this(&m->flag);
    if (battle_config.pk_mode)
        m->flag.set(MapFlag::PVP, 1);
    MapCell *gat_m = reinterpret_cast<MapCell *>(&gat_v[4]);
    std::copy(gat_m, gat_m + s, &m->gat[0]);

    size_t bxs = (xs + BLOCK_SIZE - 1) / BLOCK_SIZE;
    size_t bys = (ys + BLOCK_SIZE - 1) / BLOCK_SIZE;
    m->blocks.reset(bxs, bys);

    return true;
}

/*==========================================
 * 全てのmapデータを読み込む
 *------------------------------------------
 */
static
bool map_readallmap(void)
{
    // I am increasingly of the opinion that this needs to be moved earlier.

    int maps_removed = 0;
    int num = 0;

    for (auto& mit : maps_db)
    {
        {
            {
                map_local *ml = static_cast<map_local *>(mit.second.get());
                if (!map_readmap(ml, num, mit.first))
                {
                    // Can't remove while implicitly iterating,
                    // and I don't feel like explicitly iterating.
                    //map_delmap(map[i].name);
                    maps_removed++;
                }
                else
                    num++;
            }
        }
    }

    PRINTF("\rMaps Loaded: %-65zu\n"_fmt, maps_db.size());
    if (maps_removed)
    {
        PRINTF("Cowardly refusing to keep going after removing %d maps.\n"_fmt,
                maps_removed);
        return false;
    }

    return true;
}

/*==========================================
 * 読み込むmapを追加する
 *------------------------------------------
 */
static
void map_addmap(MapName mapname)
{
    if (mapname == "clear"_s)
    {
        maps_db.clear();
        return;
    }

    auto newmap = make_unique<map_local>();
    newmap->name_ = mapname;
    // novice challenge: figure out why this is necessary,
    // and why the previous version worked
    MapName name = newmap->name_;
    maps_db.put(name, std::move(newmap));
}

/*==========================================
 * 読み込むmapを削除する
 *------------------------------------------
 */
void map_delmap(MapName mapname)
{
    if (mapname == "all"_s)
    {
        maps_db.clear();
        return;
    }

    maps_db.put(mapname, nullptr);
}

constexpr int LOGFILE_SECONDS_PER_CHUNK_SHIFT = 10;

static
std::unique_ptr<io::AppendFile> map_logfile;
static
AString map_logfile_name;
static
long map_logfile_index;

static
void map_close_logfile(void)
{
    if (map_logfile)
    {
        AString filename = STRPRINTF("%s.%ld"_fmt, map_logfile_name, map_logfile_index);
        const char *args[] =
        {
            "gzip",
            "-f",
            filename.c_str(),
            NULL
        };
        char **argv = const_cast<char **>(args);

        map_logfile.reset();

        if (!fork())
        {
            execvp("gzip", argv);
            _exit(1);
        }
        wait(NULL);
    }
}

static
void map_start_logfile(long index)
{
    map_logfile_index = index;

    AString filename_buf = STRPRINTF(
            "%s.%ld"_fmt,
            map_logfile_name,
            map_logfile_index);
    map_logfile = make_unique<io::AppendFile>(filename_buf);
    if (!map_logfile->is_open())
    {
        map_logfile.reset();
        perror(map_logfile_name.c_str());
    }
}

static
void map_set_logfile(AString filename)
{
    struct timeval tv;

    map_logfile_name = std::move(filename);
    gettimeofday(&tv, NULL);

    map_start_logfile(tv.tv_sec >> LOGFILE_SECONDS_PER_CHUNK_SHIFT);

    MAP_LOG("log-start v5"_fmt);
}

void map_log(XString line)
{
    if (!map_logfile)
        return;

    time_t t = TimeT::now();
    long i = t >> LOGFILE_SECONDS_PER_CHUNK_SHIFT;

    if (i != map_logfile_index)
    {
        map_close_logfile();
        map_start_logfile(i);
    }

    log_with_timestamp(*map_logfile, line);
}

/*==========================================
 * 設定ファイルを読み込む
 *------------------------------------------
 */
static
bool map_config_read(ZString cfgName)
{
    struct hostent *h = NULL;

    io::ReadFile in(cfgName);
    if (!in.is_open())
    {
        PRINTF("Map configuration file not found at: %s\n"_fmt, cfgName);
        return false;
    }

    bool rv = true;
    AString line;
    while (in.getline(line))
    {
        if (is_comment(line))
            continue;
        XString w1;
        ZString w2;
        if (!config_split(line, &w1, &w2))
        {
            PRINTF("Bad config line: %s\n"_fmt, line);
            rv = false;
            continue;
        }
        if (w1 == "userid"_s)
        {
            AccountName name = stringish<AccountName>(w2);
            chrif_setuserid(name);
        }
        else if (w1 == "passwd"_s)
        {
            AccountPass pass = stringish<AccountPass>(w2);
            chrif_setpasswd(pass);
        }
        else if (w1 == "char_ip"_s)
        {
            h = gethostbyname(w2.c_str());
            IP4Address w2ip;
            if (h != NULL)
            {
                w2ip = IP4Address({
                        static_cast<uint8_t>(h->h_addr[0]),
                        static_cast<uint8_t>(h->h_addr[1]),
                        static_cast<uint8_t>(h->h_addr[2]),
                        static_cast<uint8_t>(h->h_addr[3]),
                });
                PRINTF("Character server IP address : %s -> %s\n"_fmt,
                        w2, w2ip);
            }
            else
            {
                PRINTF("Bad IP value: %s\n"_fmt, line);
                return false;
            }
            chrif_setip(w2ip);
        }
        else if (w1 == "char_port"_s)
        {
            chrif_setport(atoi(w2.c_str()));
        }
        else if (w1 == "map_ip"_s)
        {
            h = gethostbyname(w2.c_str());
            IP4Address w2ip;
            if (h != NULL)
            {
                w2ip = IP4Address({
                        static_cast<uint8_t>(h->h_addr[0]),
                        static_cast<uint8_t>(h->h_addr[1]),
                        static_cast<uint8_t>(h->h_addr[2]),
                        static_cast<uint8_t>(h->h_addr[3]),
                });
                PRINTF("Map server IP address : %s -> %s\n"_fmt,
                        w2, w2ip);
            }
            else
            {
                PRINTF("Bad IP value: %s\n"_fmt, line);
                return false;
            }
            clif_setip(w2ip);
        }
        else if (w1 == "map_port"_s)
        {
            clif_setport(atoi(w2.c_str()));
        }
        else if (w1 == "map"_s)
        {
            MapName name = VString<15>(w2);
            map_addmap(name);
        }
        else if (w1 == "delmap"_s)
        {
            MapName name = VString<15>(w2);
            map_delmap(name);
        }
        else if (w1 == "npc"_s)
        {
            npc_addsrcfile(w2);
        }
        else if (w1 == "delnpc"_s)
        {
            npc_delsrcfile(w2);
        }
        else if (w1 == "autosave_time"_s)
        {
            autosave_time = std::chrono::seconds(atoi(w2.c_str()));
            if (autosave_time <= interval_t::zero())
                autosave_time = DEFAULT_AUTOSAVE_INTERVAL;
        }
        else if (w1 == "motd_txt"_s)
        {
            motd_txt = w2;
        }
        else if (w1 == "mapreg_txt"_s)
        {
            mapreg_txt = w2;
        }
        else if (w1 == "gm_log"_s)
        {
            gm_log = std::move(w2);
        }
        else if (w1 == "log_file"_s)
        {
            map_set_logfile(w2);
        }
        else if (w1 == "import"_s)
        {
            rv &= map_config_read(w2);
        }
    }

    return rv;
}

static
void cleanup_sub(dumb_ptr<block_list> bl)
{
    nullpo_retv(bl);

    switch (bl->bl_type)
    {
        case BL::PC:
            map_delblock(bl);  // There is something better...
            break;
        case BL::NPC:
            npc_delete(bl->is_npc());
            break;
        case BL::MOB:
            mob_delete(bl->is_mob());
            break;
        case BL::ITEM:
            map_clearflooritem(bl->bl_id);
            break;
        case BL::SPELL:
            spell_free_invocation(bl->is_spell());
            break;
    }
}

/*==========================================
 * map鯖終了時処理
 *------------------------------------------
 */
void term_func(void)
{
    for (auto& mit : maps_db)
    {
        if (!mit.second->gat)
            continue;
        map_local *map_id = static_cast<map_local *>(mit.second.get());

        map_foreachinarea(cleanup_sub,
                map_id,
                0, 0,
                map_id->xs, map_id->ys,
                BL::NUL);
    }

    for (io::FD i : iter_fds())
        delete_session(get_session(i));

    map_removenpc();

    maps_db.clear();

    do_final_script();
    do_final_itemdb();
    do_final_storage();

    map_close_logfile();
}

int compare_item(struct item *a, struct item *b)
{
    return (a->nameid == b->nameid);
}

static
bool map_confs(XString key, ZString value)
{
    if (key == "map_conf"_s)
        return map_config_read(value);
    if (key == "battle_conf"_s)
        return battle_config_read(value);
    if (key == "atcommand_conf"_s)
        return atcommand_config_read(value);

    if (key == "item_db"_s)
        return itemdb_readdb(value);
    if (key == "mob_db"_s)
        return mob_readdb(value);
    if (key == "mob_skill_db"_s)
        return mob_readskilldb(value);
    if (key == "skill_db"_s)
        return skill_readdb(value);
    if (key == "magic_conf"_s)
        return load_magic_file_v2(value);

    if (key == "resnametable"_s)
        return load_resnametable(value);
    if (key == "const_db"_s)
        return read_constdb(value);
    PRINTF("unknown map conf key: %s\n"_fmt, AString(key));
    return false;
}

/*======================================================
 * Map-Server Init and Command-line Arguments [Valaris]
 *------------------------------------------------------
 */
int do_init(Slice<ZString> argv)
{
    ZString argv0 = argv.pop_front();
    runflag &= magic_init0();

    bool loaded_config_yet = false;
    while (argv)
    {
        ZString argvi = argv.pop_front();
        if (argvi.startswith('-'))
        {
            if (argvi == "--help"_s)
            {
                PRINTF("Usage: %s [--help] [--version] [--write_atcommand_config outfile] [files...]\n"_fmt,
                        argv0);
                exit(0);
            }
            else if (argvi == "--version"_s)
            {
                PRINTF("%s\n"_fmt, CURRENT_VERSION_STRING);
                exit(0);
            }
            else if (argvi == "--write-atcommand-config"_s)
            {
                if (!argv)
                {
                    PRINTF("Missing argument\n"_fmt);
                    exit(1);
                }
                ZString filename = argv.pop_front();
                atcommand_config_write(filename);
                exit(0);
            }
            else
            {
                FPRINTF(stderr, "Unknown argument: %s\n"_fmt, argvi);
                runflag = false;
            }
        }
        else
        {
            loaded_config_yet = true;
            runflag &= load_config_file(argvi, map_confs);
        }
    }

    if (!loaded_config_yet)
        runflag &= load_config_file("conf/tmwa-map.conf"_s, map_confs);

    battle_config_check();
    runflag &= map_readallmap();

    do_init_chrif();
    do_init_clif();
    do_init_mob2();
    do_init_script();

    runflag &= do_init_npc();
    do_init_pc();
    do_init_party();

    npc_event_do_oninit();     // npcのOnInitイベント実行

    if (battle_config.pk_mode == 1)
        PRINTF("The server is running in " SGR_BOLD SGR_RED "PK Mode" SGR_RESET "\n"_fmt);

    PRINTF("The map-server is " SGR_BOLD SGR_GREEN "ready" SGR_RESET " (Server is listening on the port %d).\n\n"_fmt,
            clif_getport());

    return 0;
}

int map_scriptcont(dumb_ptr<map_session_data> sd, BlockId id)
{
    dumb_ptr<block_list> bl = map_id2bl(id);

    if (!bl)
        return 0;

    switch (bl->bl_type)
    {
        case BL::NPC:
            return npc_scriptcont(sd, id);
        case BL::SPELL:
            spell_execute_script(bl->is_spell());
            break;
    }

    return 0;
}
