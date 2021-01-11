#include <algorithm>
#include <iostream>
#include <map>
#include <stdint.h>
#include <unordered_set>
#include <vector>

#include "Console.h"
#include "Core.h"
#include "Export.h"
#include "MiscUtils.h"
#include "PluginManager.h"
#include "modules/EventManager.h"
#include "modules/Gui.h"
#include "modules/Job.h"
#include "modules/MapCache.h"
#include "modules/Maps.h"
#include "modules/Units.h"
#include "modules/World.h"
#include "uicommon.h"

#include "df/announcements.h"
#include "df/block_square_event_frozen_liquidst.h"
#include "df/construction.h"
#include "df/general_ref_unit_workerst.h"
#include "df/report.h"
#include "df/ui_look_list.h"
#include "df/unit.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/world.h"
#include "df/d_init.h"

using MapExtras::MapCache;

using std::string;
using std::vector;

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("funtools");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(world);
REQUIRE_GLOBAL(process_dig);
REQUIRE_GLOBAL(ui);
REQUIRE_GLOBAL(ui_look_list);
REQUIRE_GLOBAL(ui_look_cursor);
REQUIRE_GLOBAL(cursor);
REQUIRE_GLOBAL(d_init);

void onDig(color_ostream& out, void* ptr);
EventManager::EventHandler digHandler(onDig, 0);

bool nodigcancel_state = false;

command_result nodigcancel(color_ostream& out, vector<string>& params);

struct dig_designation
{
    int block_idx, x, y;
    df::tile_dig_designation dig_type;
};

bool nodigcancel_init = false;
vector<dig_designation> saved_dig;
vector<df::job*> saved_dig_jobs;
//vector<dig_designation> fixes;
int last_dig_cancel_announce_id = -1;
int last_dig_cancel_repeats = -1;

uint32_t recenter_bit;
void nodigcancel_update(color_ostream& out);

//TODO: save/load this from file
std::unordered_set<string> forbid_freshly_mined;

command_result nodigcancel(color_ostream& out, vector <string>& parameters)
{
    CoreSuspender alice;
    if (parameters.size() == 1 && (parameters[0] == "0" || parameters[0] == "1"))
    {
        auto& flags = d_init->announcements.flags[announcement_type::DIG_CANCEL_DAMP];
        if (parameters[0] == "0")
        {
            nodigcancel_state = 0;
            flags.bits.RECENTER = recenter_bit;
        }
        else
        {
            nodigcancel_state = 1;            
            recenter_bit = flags.bits.RECENTER;
            flags.bits.RECENTER = 0;
        }
        out.print("nodigcancel %sactivated.\n", (nodigcancel_state ? "" : "de"));
    }
    else
    {
        out.print("Write 1 or 0.\n");
    }

    return CR_OK;
}

void nodigcancel_save(color_ostream& out)
{
    //Save digging designations
    saved_dig.clear();

    for (size_t i = 0; i < world->map.map_blocks.size(); ++i)
    {
        df::map_block* bl = world->map.map_blocks[i];
        if (!bl->flags.bits.designated)
            continue;

        for (int x = 0; x < 16; x++)
            for (int y = 0; y < 16; y++)
            {
                df::tile_dig_designation dig = bl->designation[x][y].bits.dig;
                if (dig != tile_dig_designation::No)
                {
                    df::tiletype tt = bl->tiletype[x][y];
                    df::tiletype_material ttm = ENUM_ATTR(tiletype, material, tt);
                    df::tiletype_shape tts = ENUM_ATTR(tiletype, shape, tt);
                    if (ttm != tiletype_material::TREE && tts != tiletype_shape::SHRUB)
                    {
                        dig_designation dd = { i, x, y, dig };
                        saved_dig.push_back(dd);
                    }
                }
            }
    }

    //Save digging jobs
    saved_dig_jobs.clear();

    for each (auto u in world->units.active)
    {
        if (!Units::isCitizen(u))
            continue;

        if (!DFHack::Units::isActive(u))
            continue;

        if (u->job.current_job != NULL && u->job.current_job->job_type == job_type::Dig)
        {
            saved_dig_jobs.push_back(Job::cloneJobStruct(u->job.current_job, true));
        }
    }
}

int chebyshev_dist(df::coord a, df::coord b)
{
    return max(max(abs(a.x - b.x), abs(a.y - b.y)), abs(a.z - b.z));
}

bool isWall(tiletype::tiletype t)
{
    return t == tiletype::StoneWall || t == tiletype::SoilWall || t == tiletype::MineralWall;
}

//void nodigcancel_fix_on_fix(color_ostream& out)
//{
//    for (size_t i = 0; i < fixes.size(); i++)
//    {
//        auto f = fixes[i];
//        auto bl = world->map.map_blocks[f.block_idx];
//        auto dig = bl->designation[f.x][f.y].bits.dig;
//        auto ttype = bl->tiletype[f.x][f.y];
//
//        if (!isWall(ttype))
//        {
//            if (dig != tile_dig_designation::No)
//            {
//                bl->designation[f.x][f.y].bits.dig = tile_dig_designation::No;                
//            }
//            vector_erase_at(fixes, i);
//            i--;
//        }
//    }    
//}

void nodigcancel_fix(color_ostream& out)
{
    //Already assigned (usually adjacent to already digged tiles) dig jobs
    //These jobs will complement bl->designation[][].bits.dig tiles
    unordered_set<df::coord> jobDigLocations;
    for (df::job_list_link* link = &world->jobs.list; link != NULL; link = link->next) {
        if (link->item == NULL)
            continue;

        if (link->item->job_type != job_type::Dig &&
            link->item->job_type != job_type::CarveUpwardStaircase &&
            link->item->job_type != job_type::CarveDownwardStaircase &&
            link->item->job_type != job_type::CarveUpDownStaircase &&
            link->item->job_type != job_type::CarveRamp &&
            link->item->job_type != job_type::DigChannel)
            continue;

        jobDigLocations.insert(link->item->pos);
    }

    for each (auto sd in saved_dig)
    {
        auto bl = world->map.map_blocks[sd.block_idx];
        auto dig = bl->designation[sd.x][sd.y].bits.dig;
        auto ttype = bl->tiletype[sd.x][sd.y];        

        if (sd.dig_type != dig && isWall(ttype))
        {
            df::coord c;
            c.x = bl->map_pos.x + sd.x;
            c.y = bl->map_pos.y + sd.y;
            c.z = bl->map_pos.z;

            if (jobDigLocations.count(c) == 1) //changed tile already has a dig job, no need to fix
                continue;

            out.print("Fixing back dig designation at %d %d %d (%d %d %d)...\n", sd.block_idx, sd.x, sd.y, c.x, c.y, c.z);

            //Fix back the designation           
            bl->designation[sd.x][sd.y].bits.dig = sd.dig_type;
            bl->flags.bits.designated = true;
            //out.print("... fixed by revert designation\n");

            //fixes.push_back(sd);

            //Designation is back, but the job was canceled anyway
            //We need to find the digger somehow
            for each (auto u in world->units.active)
            {
                if (!Units::isCitizen(u))
                    continue;

                if (!DFHack::Units::isActive(u))
                    continue;

                if (chebyshev_dist(u->pos, c) <= 2) //nearby digger...
                {
                    bool found_digger = false;
                    for each (auto job in saved_dig_jobs)
                    {
                        if (Job::getWorker(job)->id == u->id && (u->job.current_job == NULL || u->job.current_job->id != job->id) &&
                            chebyshev_dist(job->pos, c) == 1) //...who had nearby dig job
                        {
                            //TODO: ideally we want to set back his canceled job, but thats rather tricky,
                            //so lets just cancel his new pathfinding for now if he has already something other to do
                            //(his new job will be canceled too)
                            u->path.path.x.clear();
                            u->path.path.y.clear();
                            u->path.path.z.clear();
                            u->path.goal = unit_path_goal::None;
                            u->path.dest.x = -1;
                            u->path.dest.y = -1;
                            u->path.dest.z = -1;
                            u->job.current_job == NULL;
                            *process_dig = true;

                            out.print("resetted nearby dwarf work\n");

                            found_digger = true;
                            break;
                        }
                    }

                    if (found_digger)
                        break;
                }
            }
        }
    }
}

void nodigcancel_update(color_ostream& out)
{
    int n = world->status.announcements.size();
    if (n > 0)
    {
        df::report* last_ann = world->status.announcements[n - 1];
        if (last_ann->type == announcement_type::DIG_CANCEL_DAMP &&
            (last_ann->id > last_dig_cancel_announce_id || last_ann->repeat_count > last_dig_cancel_repeats))
        {
            last_dig_cancel_announce_id = last_ann->id;
            last_dig_cancel_repeats = last_ann->repeat_count;

            //Init first, so we don't attempt to fix before saving
            if (nodigcancel_init)
            {
                out.print("Detected new DIG_CANCEL_DAMP announce %d %d\n", last_ann->id, last_ann->repeat_count);
                nodigcancel_fix(out);
                World::SetPauseState(false);
            }
        }
    }

    nodigcancel_save(out);
    //nodigcancel_fix_on_fix(out);
    nodigcancel_init = true;
}

void onDig(color_ostream& out, void* ptr)
{
    CoreSuspender alice;
    df::job* job = (df::job*)ptr;
    if (job->completion_timer > 0)
        return;

    if (job->job_type != job_type::Dig &&
        job->job_type != job_type::CarveUpwardStaircase &&
        job->job_type != job_type::CarveDownwardStaircase &&
        job->job_type != job_type::CarveUpDownStaircase &&
        job->job_type != job_type::CarveRamp &&
        job->job_type != job_type::DigChannel)
        return;

    auto& items = world->items.other[items_other_id::IN_PLAY];
    for (size_t i = 0; i < items.size(); i++)
    {
        df::item* item = items[i];

        if (item->flags.bits.forbid)
            continue;

        //TODO: refine this, research where the boulder can appear (digChannel?)
        //and what age can it be? 0 or more?
        if (chebyshev_dist(item->pos, job->pos) > 1 || item->age > 3)
            continue;
        ItemTypeInfo itinfo(item);
        if (itinfo.type != item_type::BOULDER)
            continue;
        MaterialInfo minfo(item);
        if (forbid_freshly_mined.count(minfo.toString()) == 0)
            continue;

        if (item->flags.bits.in_job) //Someone already (sic!) wants to drag it somewhere
        {
            df::job* job;
            for each (auto ref in item->specific_refs)
            {
                if (ref->type == specific_ref_type::JOB)
                {
                    job = ref->data.job;
                    break;
                }
            }
            Job::removeJob(job);
        }        

        item->flags.bits.forbid = 1;
    }
}

struct dwarfmode_hook : public df::viewscreen_dwarfmodest
{
    typedef df::viewscreen_dwarfmodest interpose_base;

    static string getSelectedBoulderMat()
    {
        if (ui->main.mode != df::ui_sidebar_mode::LookAround)
            return string();

        /*auto x = cursor->x % 16;
        auto y = cursor->y % 16;
        auto bl = Maps::getTileBlock(cursor->x, cursor->y, cursor->z);       
        auto ttype = bl->tiletype[x][y];
        auto lf = bl->liquid_flow[x][y];
        auto des = bl->designation[x][y];
        auto occ = bl->occupancy[x][y];
        auto unk13 = bl->unk13[x][y];
        auto temp1 = bl->temperature_1[x][y];
        auto temp2 = bl->temperature_2[x][y];
        auto walkable = bl->walkable[x][y];*/
        //if (ttype == tiletype::Chasm || lf.bits.unk_1)
        //    return string();

        auto el = vector_get(ui_look_list->items, *ui_look_cursor);
        if (el == NULL || el->type != df::ui_look_list::T_items::T_type::Item)
            return string();
        auto item = el->data.Item;
        ItemTypeInfo itinfo(item);
        if (itinfo.type != item_type::BOULDER)
            return string();
        MaterialInfo minfo(item);
        return minfo.toString();
    }

    DEFINE_VMETHOD_INTERPOSE(void, feed, (set<df::interface_key>* input))
    {
        if (input->count(interface_key::CUSTOM_ALT_F))
        {
            auto mat = getSelectedBoulderMat();
            if (mat.empty())
                return;

            bool isEnabled = forbid_freshly_mined.count(mat) == 1;
            if (!isEnabled)
                forbid_freshly_mined.insert(mat);
            else
                forbid_freshly_mined.erase(mat);            
        }
        INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, ())
    {
        INTERPOSE_NEXT(render)();

        auto mat = getSelectedBoulderMat();
        if (mat.empty())
            return;

        bool isEnabled = forbid_freshly_mined.count(mat) == 1;

        auto dims = Gui::getDwarfmodeViewDims();
        int left_margin = dims.menu_x1 + 1;
        int x = left_margin;
        int y = dims.y2;

        auto text = isEnabled ? "autoforbid: on" : "autoforbid: off";
        OutputHotkeyString(x, y, text, "Alt+f", false, 0, COLOR_WHITE, COLOR_LIGHTRED);
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(dwarfmode_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(dwarfmode_hook, render);

DFhackCExport command_result plugin_init(color_ostream& out, vector <PluginCommand>& commands)
{
    //Reverts back dig designation if damp stone is detected and unpauses the game.
    //Caution! Use only when digging in aquifer.
    //Saves all dig designated tiles every frame and rollbacks the designation when it detects new DIG_DAMP_STONE announcement.
    //This, however, wont restore dwarf dig job, so a dwarf can be confused sometimes before he picks up the restored dig designation again.
    //(its basically the same if you just reassign the designation youself as usual)
    commands.push_back(PluginCommand("nodigcancel", "Disable dig cancellation on damp stone (useful when digging in aquifer)", nodigcancel, false,
        "Disable dig cancellation on damp stone (useful when digging in aquifer)\n"
        "Activate with 'nodigcancel 1', deactivate with 'nodigcancel 0'.\n"));

    if (!INTERPOSE_HOOK(dwarfmode_hook, render).apply() ||
        !INTERPOSE_HOOK(dwarfmode_hook, feed).apply())
        return CR_FAILURE;

    EventManager::registerListener(EventManager::EventType::JOB_COMPLETED, digHandler, plugin_self);

    is_enabled = true;
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream& out)
{
    if (nodigcancel_state)
    {
        t_gamemodes gm;
        World::ReadGameMode(gm);
        if (gm.g_mode == game_mode::DWARF)
        {
            nodigcancel_update(out);
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream& out)
{
    INTERPOSE_HOOK(dwarfmode_hook, render).remove();
    INTERPOSE_HOOK(dwarfmode_hook, feed).remove();
    EventManager::unregisterAll(plugin_self);
    return CR_OK;
}

