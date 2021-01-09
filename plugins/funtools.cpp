#include <stdint.h>
#include <iostream>
#include <map>
#include <vector>
#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "modules/EventManager.h"
#include "modules/Maps.h"
#include "modules/Units.h"
#include "modules/World.h"
#include "modules/MapCache.h"
#include "modules/Gui.h"
#include "modules/Job.h"
#include "MiscUtils.h"
#include "uicommon.h"
#include <algorithm>
#include <unordered_set>

#include "df/block_square_event_frozen_liquidst.h"
#include "df/construction.h"
#include "df/world.h"
#include "df/report.h"
#include "df/announcements.h"
#include "df/unit.h"
#include "df/general_ref_unit_workerst.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/ui_look_list.h"

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
int last_dig_cancel_announce_id = -1;
int last_dig_cancel_repeats = -1;
void nodigcancel_update(color_ostream& out);

//TODO: save/load this from file
std::unordered_set<std::string> forbid_freshly_mined;

command_result nodigcancel(color_ostream& out, vector <string>& parameters)
{
    if (parameters.size() == 1 && (parameters[0] == "0" || parameters[0] == "1"))
    {
        if (parameters[0] == "0")
            nodigcancel_state = 0;
        else
            nodigcancel_state = 1;           
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
                if (dig != df::enums::tile_dig_designation::No)
                {
                    df::tiletype tt = bl->tiletype[x][y];
                    df::tiletype_material ttm = ENUM_ATTR(tiletype, material, tt);
                    df::tiletype_shape tts = ENUM_ATTR(tiletype, shape, tt);
                    if (ttm != df::enums::tiletype_material::TREE && tts != df::enums::tiletype_shape::SHRUB)
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

        if (u->job.current_job != NULL && u->job.current_job->job_type == df::enums::job_type::Dig)
        {
            saved_dig_jobs.push_back(Job::cloneJobStruct(u->job.current_job, true));
        }
    }
}

int chebyshev_dist(df::coord a, df::coord b)
{
    return max(max(abs(a.x - b.x), abs(a.y - b.y)), abs(a.z - b.z));
}

void nodigcancel_fix(color_ostream& out)
{
    for each (auto sd in saved_dig)
    {
        auto bl = world->map.map_blocks[sd.block_idx];
        auto dig = bl->designation[sd.x][sd.y].bits.dig;
        if (sd.dig_type != dig)
        {
            df::coord c;
            c.x = bl->map_pos.x + sd.x;
            c.y = bl->map_pos.y + sd.y;
            c.z = bl->map_pos.z;

            out.print("Fixing back dig designation at %d %d %d...\n", c.x, c.y, c.z);
            
            //Fix back the designation
            bl->designation[sd.x][sd.y].bits.dig = sd.dig_type;
            bl->flags.bits.designated = true;
            out.print("... fixed by revert designation\n");            

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
        if (last_ann->type == df::enums::announcement_type::DIG_CANCEL_DAMP &&
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
    nodigcancel_init = true;    
}

void onDig(color_ostream& out, void* ptr)
{
    CoreSuspender alice;
    df::job* job = (df::job*)ptr;
    if (job->completion_timer > 0)
        return;

    if (job->job_type != df::enums::job_type::Dig &&
        job->job_type != df::enums::job_type::CarveUpwardStaircase &&
        job->job_type != df::enums::job_type::CarveDownwardStaircase &&
        job->job_type != df::enums::job_type::CarveUpDownStaircase &&
        job->job_type != df::enums::job_type::CarveRamp &&
        job->job_type != df::enums::job_type::DigChannel)
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
        if (itinfo.type != df::enums::item_type::BOULDER)
            continue;
        MaterialInfo minfo(item);
        if (forbid_freshly_mined.count(minfo.toString()) == 0)
            continue;

        item->flags.bits.forbid = 1;
    }    
}

struct dwarfmode_hook : public df::viewscreen_dwarfmodest
{
    typedef df::viewscreen_dwarfmodest interpose_base;    
    
    static std::string getSelectedBoulderMat()
    {
        if (ui->main.mode != df::ui_sidebar_mode::LookAround)
            return std::string();
        auto el = vector_get(ui_look_list->items, *ui_look_cursor);
        if (el == NULL || el->type != df::ui_look_list::T_items::T_type::Item)
            return std::string();
        auto item = el->data.Item;
        ItemTypeInfo itinfo(item);
        if (itinfo.type != df::enums::item_type::BOULDER)
            return std::string();
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

