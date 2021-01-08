#include <stdint.h>
#include <iostream>
#include <map>
#include <vector>
#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "modules/Maps.h"
#include "modules/Units.h"
#include "modules/World.h"
#include "modules/MapCache.h"
#include "modules/Gui.h"
#include "modules/Job.h"
#include <algorithm>

#include "df/block_square_event_frozen_liquidst.h"
#include "df/construction.h"
#include "df/world.h"
#include "df/report.h"
#include "df/announcements.h"
#include "df/unit.h"
#include "df/general_ref_unit_workerst.h"

using MapExtras::MapCache;

using std::string;
using std::vector;

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("funtools");
DFHACK_PLUGIN_IS_ENABLED(is_active);

REQUIRE_GLOBAL(world);

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

DFhackCExport command_result plugin_init ( color_ostream &out, vector <PluginCommand> &commands)
{   
    //Reverts back dig designation if damp stone is detected and unpauses the game.
    //Caution! Use only when digging in aquifer.
    //Saves all dig designated tiles every frame and rollbacks the designation when it detects new DIG_DAMP_STONE announcement.
    //This, however, wont restore dwarf dig job, so a dwarf can be confused sometimes before he picks up the restored dig designation again.
    //(its basically the same if you just reassign the designation youself as usual)
    commands.push_back(PluginCommand("nodigcancel", "Disable dig cancellation on damp stone (useful when digging in aquifer)", nodigcancel, false,
        "Disable dig cancellation on damp stone (useful when digging in aquifer)\n"
        "Activate with 'nodigcancel 1', deactivate with 'nodigcancel 0'.\n"));
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate ( color_ostream &out )
{   
    t_gamemodes gm;
    World::ReadGameMode(gm);
    if(gm.g_mode == game_mode::DWARF)
    {
        if (nodigcancel_state)
        {
            nodigcancel_update(out);         
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    return CR_OK;
}

command_result nodigcancel(color_ostream& out, vector <string>& parameters)
{
    if (parameters.size() == 1 && (parameters[0] == "0" || parameters[0] == "1"))
    {
        if (parameters[0] == "0")
            nodigcancel_state = 0;
        else
            nodigcancel_state = 1;   
        is_active = nodigcancel_state;
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

int nodigcancel_dist(df::coord a, df::coord b)
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

                if (nodigcancel_dist(u->pos, c) <= 2) //nearby digger...
                {
                    bool found_digger = false;
                    for each (auto job in saved_dig_jobs)
                    {
                        if (Job::getWorker(job)->id == u->id && (u->job.current_job == NULL || u->job.current_job->id != job->id) &&
                            nodigcancel_dist(job->pos, c) == 1) //...who had nearby dig job
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


