/** \file gen-cave.c
	\brief Generation of dungeon levels
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 * Copyright (c) 2013 Erik Osheim, Nick McConnell
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 *
 * In this file, we use the SQUARE_WALL flags to cave->info, which should only
 * be applied to granite.  SQUARE_WALL_SOLID indicates the wall should not be
 * tunnelled; SQUARE_WALL_INNER is the inward-facing wall of a room;
 * SQUARE_WALL_OUTER is the outer wall of a room.
 *
 * We use SQUARE_WALL_SOLID to prevent multiple corridors from piercing a wall
 * in two adjacent locations, which would be messy, and SQUARE_WALL_OUTER
 * to indicate which walls surround rooms, and may thus be pierced by corridors
 * entering or leaving the room.
 *
 * Note that a tunnel which attempts to leave a room near the edge of the
 * dungeon in a direction toward that edge will cause "silly" wall piercings,
 * but will have no permanently incorrect effects, as long as the tunnel can
 * eventually exit from another side. And note that the wall may not come back
 * into the room by the hole it left through, so it must bend to the left or
 * right and then optionally re-enter the room (at least 2 grids away). This is
 * not a problem since every room that is large enough to block the passage of
 * tunnels is also large enough to allow the tunnel to pierce the room itself
 * several times.
 *
 * Note that no two corridors may enter a room through adjacent grids, they
 * must either share an entryway or else use entryways at least two grids
 * apart. This prevents large (or "silly") doorways.
 *
 * Traditionally, to create rooms in the dungeon, it was divided up into
 * "blocks" of 11x11 grids each, and all rooms were required to occupy a
 * rectangular group of blocks.  As long as each room type reserved a
 * sufficient number of blocks, the room building routines would not need to
 * check bounds. Note that in classic generation most of the normal rooms
 * actually only use 23x11 grids, and so reserve 33x11 grids.
 *
 * Note that a lot of the original motivation for the block system was the
 * fact that there was only one size of map available, 22x66 grids, and the
 * dungeon level was divided up into nine of these in three rows of three.
 * Now that the map can be resized and enlarged, and dungeon levels themselves
 * can be different sizes, much of this original motivation has gone.  Blocks
 * can still be used, but different cave profiles can set their own block
 * sizes.  The classic generation method still uses the traditional blocks; the
 * main motivation for using blocks now is for the aesthetic effect of placing
 * rooms on a grid.
 */

#include "angband.h"
#include "cave.h"
#include "dungeon.h"
#include "files.h"
#include "game-event.h"
#include "generate.h"
#include "init.h"
#include "math.h"
#include "mon-make.h"
#include "mon-spell.h"
#include "parser.h"
#include "store.h"
#include "tables.h"
#include "trap.h"
#include "z-queue.h"
#include "z-type.h"

static bool square_is_granite_with_flag(struct cave *c, int y, int x, int flag)
{
	if (c->feat[y][x] != FEAT_GRANITE) return FALSE;
	if (!sqinfo_has(c->info[y][x], flag)) return FALSE;

	return TRUE;
}

/**
 * Places a streamer of rock through dungeon.
 *
 * Note that their are actually six different terrain features used to
 * represent streamers. Three each of magma and quartz, one for basic vein, one
 * with hidden gold, and one with known gold. The hidden gold types are
 * currently unused.
 */
static void build_streamer(struct cave *c, int feat, int chance)
{
    int i, tx, ty;
    int y, x, dir;

    /* Hack -- Choose starting point */
    y = rand_spread(c->height / 2, 10);
    x = rand_spread(c->width / 2, 15);

    /* Choose a random direction */
    dir = ddd[randint0(8)];

    /* Place streamer into dungeon */
    while (TRUE) {
		/* One grid per density */
		for (i = 0; i < dun->profile->str.den; i++) {
			int d = dun->profile->str.rng;

			/* Pick a nearby grid */
			find_nearby_grid(c, &ty, y, d, &tx, x, d);

			/* Only convert walls */
			if (square_isrock(c, ty, tx)) {
				/* Turn the rock into the vein type */
				square_set_feat(c, ty, tx, feat);

				/* Sometimes add known treasure */
				if (one_in_(chance)) upgrade_mineral(c, ty, tx);
			}
		}

		/* Advance the streamer */
		y += ddy[dir];
		x += ddx[dir];

		/* Stop at dungeon edge */
		if (!square_in_bounds(c, y, x)) break;
    }
}


/**
 * Constructs a tunnel between two points
 *
 * This function must be called BEFORE any streamers are created, since we use
 * granite with the special SQUARE_WALL flags to keep track of legal places for
 * corridors to pierce rooms.
 *
 * We queue the tunnel grids to prevent door creation along a corridor which
 * intersects itself.
 *
 * We queue the wall piercing grids to prevent a corridor from leaving
 * a room and then coming back in through the same entrance.
 *
 * We pierce grids which are outer walls of rooms, and when we do so, we change
 * all adjacent outer walls of rooms into solid walls so that no two corridors
 * may use adjacent grids for exits.
 *
 * The solid wall check prevents corridors from chopping the corners of rooms
 * off, as well as silly door placement, and excessively wide room entrances.
 */
static void build_tunnel(struct cave *c, int row1, int col1, int row2, int col2)
{
    int i, y, x;
    int tmp_row, tmp_col;
    int row_dir, col_dir;
    int start_row, start_col;
    int main_loop_count = 0;

    /* Used to prevent excessive door creation along overlapping corridors. */
    bool door_flag = FALSE;
	
    /* Reset the arrays */
    dun->tunn_n = 0;
    dun->wall_n = 0;
	
    /* Save the starting location */
    start_row = row1;
    start_col = col1;

    /* Start out in the correct direction */
    correct_dir(&row_dir, &col_dir, row1, col1, row2, col2);

    /* Keep going until done (or bored) */
    while ((row1 != row2) || (col1 != col2)) {
		/* Mega-Hack -- Paranoia -- prevent infinite loops */
		if (main_loop_count++ > 2000) break;

		/* Allow bends in the tunnel */
		if (randint0(100) < dun->profile->tun.chg) {
			/* Get the correct direction */
			correct_dir(&row_dir, &col_dir, row1, col1, row2, col2);

			/* Random direction */
			if (randint0(100) < dun->profile->tun.rnd)
				rand_dir(&row_dir, &col_dir);
		}

		/* Get the next location */
		tmp_row = row1 + row_dir;
		tmp_col = col1 + col_dir;

		while (!square_in_bounds(c, tmp_row, tmp_col)) {
			/* Get the correct direction */
			correct_dir(&row_dir, &col_dir, row1, col1, row2, col2);

			/* Random direction */
			if (randint0(100) < dun->profile->tun.rnd)
				rand_dir(&row_dir, &col_dir);

			/* Get the next location */
			tmp_row = row1 + row_dir;
			tmp_col = col1 + col_dir;
		}


		/* Avoid the edge of the dungeon */
		if (square_isperm(c, tmp_row, tmp_col)) continue;

		/* Avoid "solid" granite walls */
		if (square_is_granite_with_flag(c, tmp_row, tmp_col, 
										SQUARE_WALL_SOLID)) 
			continue;

		/* Pierce "outer" walls of rooms */
		if (square_is_granite_with_flag(c, tmp_row, tmp_col, 
										SQUARE_WALL_OUTER)) {
			/* Get the "next" location */
			y = tmp_row + row_dir;
			x = tmp_col + col_dir;

			/* Stay in bounds */
			if (!square_in_bounds(c, y, x)) continue;
 
			/* Hack -- Avoid solid permanent walls */
			if (square_isperm(c, y, x)) continue;

			/* Hack -- Avoid outer/solid granite walls */
			if (square_is_granite_with_flag(c, y, x, SQUARE_WALL_OUTER)) 
				continue;
			if (square_is_granite_with_flag(c, y, x, SQUARE_WALL_SOLID)) 
				continue;

			/* Accept this location */
			row1 = tmp_row;
			col1 = tmp_col;

			/* Save the wall location */
			if (dun->wall_n < WALL_MAX) {
				dun->wall[dun->wall_n].y = row1;
				dun->wall[dun->wall_n].x = col1;
				dun->wall_n++;
			}

			/* Forbid re-entry near this piercing */
			for (y = row1 - 1; y <= row1 + 1; y++)
				for (x = col1 - 1; x <= col1 + 1; x++)
					if (square_is_granite_with_flag(c, y, x, SQUARE_WALL_OUTER))
						set_marked_granite(c, y, x, SQUARE_WALL_SOLID);

		} else if (sqinfo_has(c->info[tmp_row][tmp_col], SQUARE_ROOM)) {
			/* Travel quickly through rooms */
			/* Accept the location */
			row1 = tmp_row;
			col1 = tmp_col;

		} else if (tf_has(f_info[c->feat[tmp_row][tmp_col]].flags, TF_GRANITE)||
				   tf_has(f_info[c->feat[tmp_row][tmp_col]].flags, TF_PERMANENT)){
			/* Tunnel through all other walls */
			/* Accept this location */
			row1 = tmp_row;
			col1 = tmp_col;

			/* Save the tunnel location */
			if (dun->tunn_n < TUNN_MAX) {
				dun->tunn[dun->tunn_n].y = row1;
				dun->tunn[dun->tunn_n].x = col1;
				dun->tunn_n++;
			}

			/* Allow door in next grid */
			door_flag = FALSE;

		} else {
			/* Handle corridor intersections or overlaps */
			/* Accept the location */
			row1 = tmp_row;
			col1 = tmp_col;

			/* Collect legal door locations */
			if (!door_flag) {
				/* Save the door location */
				if (dun->door_n < DOOR_MAX) {
					dun->door[dun->door_n].y = row1;
					dun->door[dun->door_n].x = col1;
					dun->door_n++;
				}

				/* No door in next grid */
				door_flag = TRUE;
			}

			/* Hack -- allow pre-emptive tunnel termination */
			if (randint0(100) >= dun->profile->tun.con) {
				/* Distance between row1 and start_row */
				tmp_row = row1 - start_row;
				if (tmp_row < 0) tmp_row = (-tmp_row);

				/* Distance between col1 and start_col */
				tmp_col = col1 - start_col;
				if (tmp_col < 0) tmp_col = (-tmp_col);

				/* Terminate the tunnel */
				if ((tmp_row > 10) || (tmp_col > 10)) break;
			}
		}
    }


    /* Turn the tunnel into corridor */
    for (i = 0; i < dun->tunn_n; i++) {
		/* Get the grid */
		y = dun->tunn[i].y;
		x = dun->tunn[i].x;

		/* Clear previous contents, add a floor */
		square_set_feat(c, y, x, FEAT_FLOOR);
    }


    /* Apply the piercings that we found */
    for (i = 0; i < dun->wall_n; i++) {
		/* Get the grid */
		y = dun->wall[i].y;
		x = dun->wall[i].x;

		/* Convert to floor grid */
		square_set_feat(c, y, x, FEAT_FLOOR);

		/* Place a random door */
		if (randint0(100) < dun->profile->tun.pen)
			place_random_door(c, y, x);
    }
}

/**
 * Count the number of corridor grids adjacent to the given grid.
 *
 * This routine currently only counts actual "empty floor" grids which are not
 * in rooms.
 *
 * TODO: count stairs, open doors, closed doors?
 */
static int next_to_corr(struct cave *c, int y1, int x1)
{
    int i, k = 0;
    assert(square_in_bounds(c, y1, x1));

    /* Scan adjacent grids */
    for (i = 0; i < 4; i++) {
		/* Extract the location */
		int y = y1 + ddy_ddd[i];
		int x = x1 + ddx_ddd[i];

		/* Count only floors which aren't part of rooms */
		if (square_isfloor(c, y, x) && !square_isroom(c, y, x)) k++;
    }

    /* Return the number of corridors */
    return k;
}

/**
 * Returns whether a doorway can be built in a space.
 *
 * To have a doorway, a space must be adjacent to at least two corridors and be
 * between two walls.
 */
static bool possible_doorway(struct cave *c, int y, int x)
{
    assert(square_in_bounds(c, y, x));
    if (next_to_corr(c, y, x) < 2)
		return FALSE;
    else if (square_isstrongwall(c, y - 1, x) && square_isstrongwall(c, y + 1, x))
		return TRUE;
    else if (square_isstrongwall(c, y, x - 1) && square_isstrongwall(c, y, x + 1))
		return TRUE;
    else
		return FALSE;
}


/**
 * Places door at y, x position if at least 2 walls found
 */
static void try_door(struct cave *c, int y, int x)
{
    assert(square_in_bounds(c, y, x));

    if (square_isstrongwall(c, y, x)) return;
    if (square_isroom(c, y, x)) return;

    if (randint0(100) < dun->profile->tun.jct && possible_doorway(c, y, x))
		place_random_door(c, y, x);
}


/**
 * Generate a new dungeon level.
 */
struct cave *classic_gen(struct player *p) {
    int i, j, k, y, x, y1, x1;
    int by, bx = 0, tby, tbx, key, rarity, built;
    int num_rooms, size_percent;
    int dun_unusual = dun->profile->dun_unusual;

    bool **blocks_tried;
	struct cave *c;

    /* This code currently does nothing - see comments below */
    i = randint1(10) + p->depth / 24;
    if (is_quest(p->depth)) size_percent = 100;
    else if (i < 2) size_percent = 75;
    else if (i < 3) size_percent = 80;
    else if (i < 4) size_percent = 85;
    else if (i < 5) size_percent = 90;
    else if (i < 6) size_percent = 95;
    else size_percent = 100;

    /* scale the various generation variables */
    num_rooms = (dun->profile->dun_rooms * size_percent) / 100;
	dun->block_hgt = dun->profile->block_size;
	dun->block_wid = dun->profile->block_size;
	c = cave_new(DUNGEON_HGT, DUNGEON_WID);
	c->depth = p->depth;
    ROOM_LOG("height=%d  width=%d  nrooms=%d", c->height, c->width, num_rooms);

    /* Fill cave area with basic granite */
    fill_rectangle(c, 0, 0, c->height - 1, c->width - 1, 
				   FEAT_GRANITE, SQUARE_NONE);

    /* Actual maximum number of rooms on this level */
    dun->row_blocks = c->height / dun->block_hgt;
    dun->col_blocks = c->width / dun->block_wid;

    /* Initialize the room table */
	dun->room_map = mem_zalloc(dun->row_blocks * sizeof(bool*));
	for (i = 0; i < dun->row_blocks; i++)
		dun->room_map[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* Initialize the block table */
    blocks_tried = mem_zalloc(dun->row_blocks * sizeof(bool*));

	for (i = 0; i < dun->row_blocks; i++)
		blocks_tried[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* No rooms yet, pits or otherwise. */
    dun->pit_num = 0;
    dun->cent_n = 0;

    /* Build some rooms.  Note that the theoretical maximum number of rooms
	 * in this profile is currently 36, so built never reaches num_rooms,
	 * and room generation is always terminated by having tried all blocks */
    built = 0;
    while(built < num_rooms) {

		/* Count the room blocks we haven't tried yet. */
		j = 0;
		tby = 0;
		tbx = 0;
		for(by = 0; by < dun->row_blocks; by++) {
			for(bx = 0; bx < dun->col_blocks; bx++) {
				if (blocks_tried[by][bx]) continue;
				j++;
				if (one_in_(j)) {
					tby = by;
					tbx = bx;
				}
			} 
		}
		bx = tbx;
		by = tby;

		/* If we've tried all blocks we're done. */
		if (j == 0) break;

		if (blocks_tried[by][bx]) quit_fmt("generation: inconsistent blocks");

		/* Mark that we are trying this block. */
		blocks_tried[by][bx] = TRUE;

		/* Roll for random key (to be compared against a profile's cutoff) */
		key = randint0(100);

		/* We generate a rarity number to figure out how exotic to make the
		 * room. This number has a depth/DUN_UNUSUAL chance of being > 0,
		 * a depth^2/DUN_UNUSUAL^2 chance of being > 1, up to MAX_RARITY. */
		i = 0;
		rarity = 0;
		while (i == rarity && i < dun->profile->max_rarity) {
			if (randint0(dun_unusual) < 50 + c->depth / 2) rarity++;
			i++;
		}

		/* Once we have a key and a rarity, we iterate through out list of
		 * room profiles looking for a match (whose cutoff > key and whose
		 * rarity > this rarity). We try building the room, and if it works
		 * then we are done with this iteration. We keep going until we find
		 * a room that we can build successfully or we exhaust the profiles. */
		for (i = 0; i < dun->profile->n_room_profiles; i++) {
			struct room_profile profile = dun->profile->room_profiles[i];
			if (profile.rarity > rarity) continue;
			if (profile.cutoff <= key) continue;
			
			if (room_build(c, by, bx, profile, FALSE)) {
				built++;
				break;
			}
		}
    }

	for (i = 0; i < dun->row_blocks; i++){
		mem_free(blocks_tried[i]);
		mem_free(dun->room_map[i]);
	}
	mem_free(blocks_tried);
	mem_free(dun->room_map);

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, 
				   FEAT_PERM, SQUARE_NONE);

    /* Hack -- Scramble the room order */
    for (i = 0; i < dun->cent_n; i++) {
		int pick1 = randint0(dun->cent_n);
		int pick2 = randint0(dun->cent_n);
		y1 = dun->cent[pick1].y;
		x1 = dun->cent[pick1].x;
		dun->cent[pick1].y = dun->cent[pick2].y;
		dun->cent[pick1].x = dun->cent[pick2].x;
		dun->cent[pick2].y = y1;
		dun->cent[pick2].x = x1;
    }

    /* Start with no tunnel doors */
    dun->door_n = 0;

    /* Hack -- connect the first room to the last room */
    y = dun->cent[dun->cent_n-1].y;
    x = dun->cent[dun->cent_n-1].x;

    /* Connect all the rooms together */
    for (i = 0; i < dun->cent_n; i++) {
		/* Connect the room to the previous room */
		build_tunnel(c, dun->cent[i].y, dun->cent[i].x, y, x);

		/* Remember the "previous" room */
		y = dun->cent[i].y;
		x = dun->cent[i].x;
    }

    /* Place intersection doors */
    for (i = 0; i < dun->door_n; i++) {
		/* Extract junction location */
		y = dun->door[i].y;
		x = dun->door[i].x;

		/* Try placing doors */
		try_door(c, y, x - 1);
		try_door(c, y, x + 1);
		try_door(c, y - 1, x);
		try_door(c, y + 1, x);
    }

    ensure_connectedness(c);

    /* Add some magma streamers */
    for (i = 0; i < dun->profile->str.mag; i++)
		build_streamer(c, FEAT_MAGMA, dun->profile->str.mc);

    /* Add some quartz streamers */
    for (i = 0; i < dun->profile->str.qua; i++)
		build_streamer(c, FEAT_QUARTZ, dun->profile->str.qc);

    /* Place 3 or 4 down stairs near some walls */
    alloc_stairs(c, FEAT_MORE, rand_range(3, 4), 3);

    /* Place 1 or 2 up stairs near some walls */
    alloc_stairs(c, FEAT_LESS, rand_range(1, 2), 3);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(c->depth / 3, 10), 2);

    /* Put some rubble in corridors */
    alloc_objects(c, SET_CORR, TYP_RUBBLE, randint1(k), c->depth, 0);

    /* Place some traps in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_TRAP, randint1(k), c->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Pick a base number of monsters */
    i = MIN_M_ALLOC_LEVEL + randint1(8) + k;

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
		pick_and_place_distant_monster(c, loc(p->px, p->py), 0, TRUE, c->depth);

    /* Put some objects in rooms */
    alloc_objects(c, SET_ROOM, TYP_OBJECT, Rand_normal(AMT_ROOM, 3),
				  c->depth, ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_OBJECT, Rand_normal(AMT_ITEM, 3),
				  c->depth, ORIGIN_FLOOR);
    alloc_objects(c, SET_BOTH, TYP_GOLD, Rand_normal(AMT_GOLD, 3),
				  c->depth, ORIGIN_FLOOR);

    return c;
}


/* ------------------ LABYRINTH ---------------- */

/**
 * Given an adjoining wall (a wall which separates two labyrinth cells)
 * set a and b to point to the cell indices which are separated. Used by
 * labyrinth_gen().
 */
static void lab_get_adjoin(int i, int w, int *a, int *b) {
    int y, x;
    i_to_yx(i, w, &y, &x);
    if (x % 2 == 0) {
		*a = yx_to_i(y - 1, x, w);
		*b = yx_to_i(y + 1, x, w);
    } else {
		*a = yx_to_i(y, x - 1, w);
		*b = yx_to_i(y, x + 1, w);
    }
}

/**
 * Return whether (x, y) is in a tunnel.
 *
 * For our purposes a tunnel is a horizontal or vertical path, not an
 * intersection. Thus, we want the squares on either side to walls in one
 * case (e.g. up/down) and open in the other case (e.g. left/right). We don't
 * want a square that represents an intersection point.
 *
 * The high-level idea is that these are squares which can't be avoided (by
 * walking diagonally around them).
 */
static bool lab_is_tunnel(struct cave *c, int y, int x) {
    bool west = square_isopen(c, y, x - 1);
    bool east = square_isopen(c, y, x + 1);
    bool north = square_isopen(c, y - 1, x);
    bool south = square_isopen(c, y + 1, x);

    return north == south && west == east && north != west;
}


/**
 * Build a labyrinth chunk of a given height and width, optionally lit, mapped
 * and/or permanent-walled
 */
struct cave *labyrinth_chunk(int depth, int h, int w, bool lit, bool soft)
{
    int i, j, k, y, x;
    /* This is the number of squares in the labyrinth */
    int n = h * w;

    /* NOTE: 'sets' and 'walls' are too large... we only need to use about
     * 1/4 as much memory. However, in that case, the addressing math becomes
     * a lot more complicated, so let's just stick with this because it's
     * easier to read. */

    /* 'sets' tracks connectedness; if sets[i] == sets[j] then cells i and j
     * are connected to each other in the maze. */
    int *sets;

    /* 'walls' is a list of wall coordinates which we will randomize */
    int *walls;

	/* The labyrinth chunk */
	struct cave *c = cave_new(h + 2, w + 2);
	c->depth = depth;
    /* allocate our arrays */
    sets = mem_zalloc(n * sizeof(int));
    walls = mem_zalloc(n * sizeof(int));

    /* Bound with perma-rock */
    draw_rectangle(c, 0, 0, h + 1, w + 1, 
				   FEAT_PERM, SQUARE_NONE);

    /* Fill the labyrinth area with rock */
	if (soft)
		fill_rectangle(c, 1, 1, h, w, FEAT_GRANITE, SQUARE_WALL_SOLID);
	else
		fill_rectangle(c, 1, 1, h, w, FEAT_PERM, SQUARE_NONE);

    /* Initialize each wall. */
    for (i = 0; i < n; i++) {
		walls[i] = i;
		sets[i] = -1;
    }

    /* Cut out a grid of 1x1 rooms which we will call "cells" */
    for (y = 0; y < h; y += 2) {
		for (x = 0; x < w; x += 2) {
			int k = yx_to_i(y, x, w);
			sets[k] = k;
			square_set_feat(c, y + 1, x + 1, FEAT_FLOOR);
			if (lit) sqinfo_on(c->info[y + 1][x + 1], SQUARE_GLOW);
		}
    }

    /* Shuffle the walls, using Knuth's shuffle. */
    shuffle(walls, n);

    /* For each adjoining wall, look at the cells it divides. If they aren't
     * in the same set, remove the wall and join their sets.
     *
     * This is a randomized version of Kruskal's algorithm. */
    for (i = 0; i < n; i++) {
		int a, b, x, y;

		j = walls[i];

		/* If this cell isn't an adjoining wall, skip it */
		i_to_yx(j, w, &y, &x);
		if ((x < 1 && y < 1) || (x > w - 2 && y > h - 2)) continue;
		if (x % 2 == y % 2) continue;

		/* Figure out which cells are separated by this wall */
		lab_get_adjoin(j, w, &a, &b);

		/* If the cells aren't connected, kill the wall and join the sets */
		if (sets[a] != sets[b]) {
			int sa = sets[a];
			int sb = sets[b];
			square_set_feat(c, y + 1, x + 1, FEAT_FLOOR);
			if (lit) sqinfo_on(c->info[y + 1][x + 1], SQUARE_GLOW);

			for (k = 0; k < n; k++) {
				if (sets[k] == sb) sets[k] = sa;
			}
		}
    }

    /* Generate a door for every 100 squares in the labyrinth */
    for (i = n / 100; i > 0; i--) {
		/* Try 10 times to find a useful place for a door, then place it */
		for (j = 0; j < 10; j++) {
			find_empty(c, &y, &x);
			if (lab_is_tunnel(c, y, x)) break;

		}

		place_closed_door(c, y, x);
    }

    /* Unlit labyrinths will have some good items */
    if (!lit)
		alloc_objects(c, SET_BOTH, TYP_GOOD, Rand_normal(3, 2), c->depth,
					  ORIGIN_LABYRINTH);

    /* Hard (non-diggable) labyrinths will have some great items */
    if (!soft)
		alloc_objects(c, SET_BOTH, TYP_GREAT, Rand_normal(2, 1), c->depth,
					  ORIGIN_LABYRINTH);

    /* Deallocate our lists */
    mem_free(sets);
    mem_free(walls);

	return c;
}

/**
 * Build a labyrinth level.
 *
 * Note that if the function returns FALSE, a level wasn't generated.
 * Labyrinths use the dungeon level's number to determine whether to generate
 * themselves (which means certain level numbers are more likely to generate
 * labyrinths than others).
 */
struct cave *labyrinth_gen(struct player *p) {
    int i, k, y, x;
	struct cave *c;

    /* Size of the actual labyrinth part must be odd. */
    /* NOTE: these are not the actual dungeon size, but rather the size of the
     * area we're genearting a labyrinth in (which doesn't count the enclosing
     * outer walls. */
    int h = 15 + randint0(p->depth / 10) * 2;
    int w = 51 + randint0(p->depth / 10) * 2;

    /* Most labyrinths are lit */
    bool lit = randint0(p->depth) < 25 || randint0(2) < 1;

    /* Many labyrinths are known */
    bool known = lit && randint0(p->depth) < 25;

    /* Most labyrinths have soft (diggable) walls */
    bool soft = randint0(p->depth) < 35 || randint0(3) < 2;

	/* Generate the actual labyrinth */
	c = labyrinth_chunk(p->depth, h, w, lit, soft);
	if (!c) return NULL;
	c->depth = p->depth;

    /* Determine the character location */
    new_player_spot(c, p);

    /* Generate a single set of stairs up if necessary. */
    if (!cave_find(c, &y, &x, square_isupstairs))
		alloc_stairs(c, FEAT_LESS, 1, 3);

    /* Generate a single set of stairs down if necessary. */
    if (!cave_find(c, &y, &x, square_isdownstairs))
		alloc_stairs(c, FEAT_MORE, 1, 3);

    /* General some rubble, traps and monsters */
    k = MAX(MIN(c->depth / 3, 10), 2);

    /* Scale number of monsters items by labyrinth size */
    k = (3 * k * (h * w)) / (DUNGEON_HGT * DUNGEON_WID);

    /* Put some rubble in corridors */
    alloc_objects(c, SET_BOTH, TYP_RUBBLE, randint1(k), c->depth, 0);

    /* Place some traps in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_TRAP, randint1(k), c->depth, 0);

    /* Put some monsters in the dungeon */
    for (i = MIN_M_ALLOC_LEVEL + randint1(8) + k; i > 0; i--)
		pick_and_place_distant_monster(c, loc(p->px, p->py), 0, TRUE, c->depth);

    /* Put some objects/gold in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_OBJECT, Rand_normal(k * 6, 2), c->depth,
				  ORIGIN_LABYRINTH);
    alloc_objects(c, SET_BOTH, TYP_GOLD, Rand_normal(k * 3, 2), c->depth,
				  ORIGIN_LABYRINTH);
    alloc_objects(c, SET_BOTH, TYP_GOOD, randint1(2), c->depth,
				  ORIGIN_LABYRINTH);

    /* If we want the players to see the maze layout, do that now */
    if (known) wiz_light(c, FALSE);

    return c;
}


/* ---------------- CAVERNS ---------------------- */

/**
 * Initialize the dungeon array, with a random percentage of squares open.
 */
static void init_cavern(struct cave *c, int density) {
    int h = c->height;
    int w = c->width;
    int size = h * w;
	
    int count = (size * density) / 100;

    /* Fill the entire chunk with rock */
    fill_rectangle(c, 0, 0, h - 1, w - 1, FEAT_GRANITE, SQUARE_WALL_SOLID);
	
    while (count > 0) {
		int y = randint1(h - 2);
		int x = randint1(w - 2);
		if (square_isrock(c, y, x)) {
			square_set_feat(c, y, x, FEAT_FLOOR);
			count--;
		}
    }
}

/**
 * Return the number of walls (0-8) adjacent to this square.
 */
static int count_adj_walls(struct cave *c, int y, int x) {
    int yd, xd;
    int count = 0;

    for (yd = -1; yd <= 1; yd++) {
		for (xd = -1; xd <= 1; xd++) {
			if (yd == 0 && xd == 0) continue;
			if (square_isfloor(c, y + yd, x + xd)) continue;
			count++;
		}
    }

    return count;
}

/**
 * Run a single pass of the cellular automata rules (4,5) on the dungeon.
 */
static void mutate_cavern(struct cave *c) {
    int y, x;
    int h = c->height;
    int w = c->width;

    int *temp = C_ZNEW(h * w, int);

    for (y = 1; y < h - 1; y++) {
		for (x = 1; x < w - 1; x++) {
			int count = count_adj_walls(c, y, x);
			if (count > 5)
				temp[y * w + x] = FEAT_GRANITE;
			else if (count < 4)
				temp[y * w + x] = FEAT_FLOOR;
			else
				temp[y * w + x] = c->feat[y][x];
		}
    }

    for (y = 1; y < h - 1; y++) {
		for (x = 1; x < w - 1; x++) {
			square_set_feat(c, y, x, temp[y * w + x]);
		}
    }

    FREE(temp);
}

/**
 * Fill an int[] with a single value.
 */
static void array_filler(int data[], int value, int size) {
    int i;
    for (i = 0; i < size; i++) data[i] = value;
}

/**
 * Determine if we need to worry about coloring a point, or can ignore it.
 */
static int ignore_point(struct cave *c, int colors[], int y, int x) {
    int h = c->height;
    int w = c->width;
    int n = yx_to_i(y, x, w);

    if (y < 0 || x < 0 || y >= h || x >= w) return TRUE;
    if (colors[n]) return TRUE;
    //if (square_isvault(c, y, x)) return FALSE;
    if (square_ispassable(c, y, x)) return FALSE;
    if (square_isdoor(c, y, x)) return FALSE;
    return TRUE;
}

static int xds[] = {0, 0, 1, -1, -1, -1, 1, 1};
static int yds[] = {1, -1, 0, 0, -1, 1, -1, 1};

#if 0 /* XXX d_m - is this meant to be in use? */
static void glow_point(struct cave *c, int y, int x) {
    int i, j;
    for (i = -1; i <= -1; i++)
		for (j = -1; j <= -1; j++)
			sqinfo_on(c->info[y + i][x + j], SQUARE_GLOW);
}
#endif

/**
 * Color a particular point, and all adjacent points.
 */
static void build_color_point(struct cave *c, int colors[], int counts[], int y, int x, int color, bool diagonal) {
    int h = c->height;
    int w = c->width;
    int size = h * w;
    struct queue *queue = q_new(size);

    int dslimit = diagonal ? 8 : 4;

    int *added = C_ZNEW(size, int);
    array_filler(added, 0, size);

    q_push_int(queue, yx_to_i(y, x, w));

    counts[color] = 0;

    while (q_len(queue) > 0) {
		int i, y2, x2;
		int n2 = q_pop_int(queue);

		i_to_yx(n2, w, &y2, &x2);

		if (ignore_point(c, colors, y2, x2)) continue;

		colors[n2] = color;
		counts[color]++;

		/*if (lit) glow_point(c, y2, x2);*/

		for (i = 0; i < dslimit; i++) {
			int y3 = y2 + yds[i];
			int x3 = x2 + xds[i];
			int n3 = yx_to_i(y3, x3, w);
			if (ignore_point(c, colors, y3, x3)) continue;
			if (added[n3]) continue;

			q_push_int(queue, n3);
			added[n3] = 1;
		}
    }

    FREE(added);
    q_free(queue);
}

/**
 * Create a color for each "NESW contiguous" region of the dungeon.
 */
static void build_colors(struct cave *c, int colors[], int counts[], bool diagonal) {
    int y, x;
    int h = c->height;
    int w = c->width;
    int color = 1;

    for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (ignore_point(c, colors, y, x)) continue;
			build_color_point(c, colors, counts, y, x, color, diagonal);
			color++;
		}
    }
}

/**
 * Find and delete all small (<9 square) open regions.
 */
static void clear_small_regions(struct cave *c, int colors[], int counts[]) {
    int i, y, x;
    int h = c->height;
    int w = c->width;
    int size = h * w;

    int *deleted = C_ZNEW(size, int);
    array_filler(deleted, 0, size);

    for (i = 0; i < size; i++) {
		if (counts[i] < 9) {
			deleted[i] = 1;
			counts[i] = 0;
		}
    }

    for (y = 1; y < c->height - 1; y++) {
		for (x = 1; x < c->width - 1; x++) {
			i = yx_to_i(y, x, w);

			if (!deleted[colors[i]]) continue;

			colors[i] = 0;
			set_marked_granite(c, y, x, SQUARE_WALL_SOLID);
		}
    }
    FREE(deleted);
}

/**
 * Return the number of colors which have active cells.
 */
static int count_colors(int counts[], int size) {
    int i;
    int num = 0;
    for (i = 0; i < size; i++) if (counts[i] > 0) num++;
    return num;
}

/**
 * Return the first color which has one or more active cells.
 */
static int first_color(int counts[], int size) {
    int i;
    for (i = 0; i < size; i++) if (counts[i] > 0) return i;
    return -1;
}

/**
 * Find all cells of 'fromcolor' and repaint them to 'tocolor'.
 */
static void fix_colors(int colors[], int counts[], int from, int to, int size) {
    int i;
    for (i = 0; i < size; i++) if (colors[i] == from) colors[i] = to;
    counts[to] += counts[from];
    counts[from] = 0;
}

/**
 * Create a tunnel connecting a region to one of its nearest neighbors.
 * Set new_color = -1 for any neighbour, the required color for a specific one
 */
static void join_region(struct cave *c, int colors[], int counts[], int color,
	int new_color)
{
    int i;
    int h = c->height;
    int w = c->width;
    int size = h * w;

    /* Allocate a processing queue */
    struct queue *queue = q_new(size);

    /* Allocate an array to keep track of handled squares, and which square
     * we reached them from.
     */
    int *previous = C_ZNEW(size, int);
    array_filler(previous, -1, size);

    /* Push all squares of the given color onto the queue */
    for (i = 0; i < size; i++) {
		if (colors[i] == color) {
			q_push_int(queue, i);
			previous[i] = i;
		}
    }

    /* Process all squares into the queue */
    while (q_len(queue) > 0) {
		/* Get the current square and its color */
		int n = q_pop_int(queue);
		int color2 = colors[n];

		/* If we're not looking for a specific color, any new one will do */
		if ((new_color == -1) && color2 && (color2 != color))
			new_color = color2;

		/* See if we've reached a square with a new color */
		if (color2 == new_color) {
			/* Step backward through the path, turning stone to tunnel */
			while (colors[n] != color) {
				int x, y;
				i_to_yx(n, w, &y, &x);
				colors[n] = color;
				if (!square_isperm(c, y, x) && !square_isvault(c, y, x)) {
					square_set_feat(c, y, x, FEAT_FLOOR);
				}
				n = previous[n];
			}

			/* Update the color mapping to combine the two colors */
			fix_colors(colors, counts, color2, color, size);

			/* We're done now */
			break;
		}

		/* If we haven't reached a new color, add all the unprocessed adjacent
		 * squares to our queue.
		 */
		for (i = 0; i < 4; i++) {
			int y, x, n2;
			i_to_yx(n, w, &y, &x);

			/* Move to the adjacent square */
			y += yds[i];
			x += xds[i];

			/* make sure we stay inside the boundaries */
			if (y < 0 || y >= h) continue;
			if (x < 0 || x >= w) continue;

			/* If the cell hasn't already been procssed, add it to the queue */
			n2 = yx_to_i(y, x, w);
			if (previous[n2] >= 0) continue;
			q_push_int(queue, n2);
			previous[n2] = n;
		}
    }

    /* Free the memory we've allocated */
    q_free(queue);
    FREE(previous);
}


/**
 * Start connecting regions, stopping when the cave is entirely connected.
 */
static void join_regions(struct cave *c, int colors[], int counts[]) {
    int h = c->height;
    int w = c->width;
    int size = h * w;
    int num = count_colors(counts, size);

    /* While we have multiple colors (i.e. disconnected regions), join one of
     * the regions to another one.
     */
    while (num > 1) {
		int color = first_color(counts, size);
		join_region(c, colors, counts, color, -1);
		num--;
    }
}


/**
 * Make sure that all the regions of the dungeon are connected.
 *
 * This function colors each connected region of the dungeon, then uses that
 * information to join them into one conected region.
 */
void ensure_connectedness(struct cave *c) {
    int size = c->height * c->width;
    int *colors = C_ZNEW(size, int);
    int *counts = C_ZNEW(size, int);

    build_colors(c, colors, counts, TRUE);
    join_regions(c, colors, counts);

    FREE(colors);
    FREE(counts);
}


#define MAX_CAVERN_TRIES 10
/**
 * The generator's main function.
 */
struct cave *cavern_chunk(int depth, int h, int w)
{
    int i;
    int size = h * w;
    int limit = size / 13;
    int density = rand_range(25, 40);
    int times = rand_range(3, 6);

    int *colors = mem_zalloc(size * sizeof(int));
    int *counts = mem_zalloc(size * sizeof(int));

    int tries;

	struct cave *c = cave_new(h, w);
	c->depth = depth;

    ROOM_LOG("cavern h=%d w=%d size=%d density=%d times=%d", h, w, size,
			 density, times);

	/* Start trying to build caverns */
	for (tries = 0; tries < MAX_CAVERN_TRIES; tries++) {
		/* Build a random cavern and mutate it a number of times */
		init_cavern(c, density);
		for (i = 0; i < times; i++) mutate_cavern(c);

		/* If there are enough open squares then we're done */
		if (c->feat_count[FEAT_FLOOR] >= limit) {
			ROOM_LOG("cavern ok (%d vs %d)", c->feat_count[FEAT_FLOOR], limit);
			break;
		}
		ROOM_LOG("cavern failed--try again (%d vs %d)",
				 c->feat_count[FEAT_FLOOR], limit);
	}

	/* If we couldn't make a big enough cavern then fail */
	if (tries == MAX_CAVERN_TRIES) {
		cave_free(c);
		return NULL;
	}

	build_colors(c, colors, counts, FALSE);
	clear_small_regions(c, colors, counts);
	join_regions(c, colors, counts);

    mem_free(colors);
    mem_free(counts);

	return c;
}


/**
 * Make a cavern level.
 */
struct cave *cavern_gen(struct player *p) {
    int i, k;

    int h = rand_range(DUNGEON_HGT / 2, (DUNGEON_HGT * 3) / 4);
    int w = rand_range(DUNGEON_WID / 2, (DUNGEON_WID * 3) / 4);

	struct cave *c;

    if (p->depth < 15) {
		/* If we're too shallow then don't do it */
		return FALSE;

    } else {
		/* Try to build the cavern, fail gracefully */
		c = cavern_chunk(p->depth, h, w);
		if (!c) return NULL;
    }
	c->depth = p->depth;

	/* Surround the level with perma-rock */
    draw_rectangle(c, 0, 0, h - 1, w - 1, FEAT_PERM, SQUARE_NONE);

	/* Place 2-3 down stairs near some walls */
	alloc_stairs(c, FEAT_MORE, rand_range(1, 3), 3);

	/* Place 1-2 up stairs near some walls */
	alloc_stairs(c, FEAT_LESS, rand_range(1, 2), 3);

	/* General some rubble, traps and monsters */
	k = MAX(MIN(c->depth / 3, 10), 2);

	/* Scale number of monsters items by cavern size */
	k = MAX((4 * k * (h *  w)) / (DUNGEON_HGT * DUNGEON_WID), 6);

	/* Put some rubble in corridors */
	alloc_objects(c, SET_BOTH, TYP_RUBBLE, randint1(k), c->depth, 0);

	/* Place some traps in the dungeon */
	alloc_objects(c, SET_BOTH, TYP_TRAP, randint1(k), c->depth, 0);

	/* Determine the character location */
	new_player_spot(c, p);

	/* Put some monsters in the dungeon */
	for (i = randint1(8) + k; i > 0; i--)
		pick_and_place_distant_monster(c, loc(p->px, p->py), 0, TRUE, c->depth);

	/* Put some objects/gold in the dungeon */
	alloc_objects(c, SET_BOTH, TYP_OBJECT, Rand_normal(k, 2), c->depth + 5,
				  ORIGIN_CAVERN);
	alloc_objects(c, SET_BOTH, TYP_GOLD, Rand_normal(k / 2, 2), c->depth,
				  ORIGIN_CAVERN);
	alloc_objects(c, SET_BOTH, TYP_GOOD, randint0(k / 4), c->depth,
				  ORIGIN_CAVERN);

	return c;
}

/* ------------------ TOWN ---------------- */

/**
 * Builds a store at a given pseudo-location
 *
 * Currently, there is a main street horizontally through the middle of town,
 * and all the shops face it (e.g. the shops on the north side face south).
 */
static void build_store(struct cave *c, int n, int yy, int xx) {
    /* Find the "center" of the store */
    int y0 = yy * 9 + 6;
    int x0 = xx * 14 + 12;

    /* Determine the store boundaries */
    int y1 = y0 - randint1((yy == 0) ? 3 : 2);
    int y2 = y0 + randint1((yy == 1) ? 3 : 2);
    int x1 = x0 - randint1(5);
    int x2 = x0 + randint1(5);

    /* Determine door location, based on which side of the street we're on */
    int dy = (yy == 0) ? y2 : y1;
    int dx = rand_range(x1, x2);

    /* Build an invulnerable rectangular building */
    fill_rectangle(c, y1, x1, y2, x2, FEAT_PERM, SQUARE_NONE);

    /* Clear previous contents, add a store door */
    square_set_feat(c, dy, dx, FEAT_SHOP_HEAD + n);
}


/**
 * Generate the town for the first time, and place the player
 */
static void town_gen_layout(struct cave *c, struct player *p) {
    int y, x, n, k;
    int rooms[MAX_STORES];

    int n_rows = 2;
    int n_cols = (MAX_STORES + 1) / n_rows;

	/* Create walls */
	draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM,
				   SQUARE_NONE);

	/* Create some floor */
	fill_rectangle(c, 1, 1, c->height - 2, c->width - 2, FEAT_FLOOR,
				   SQUARE_NONE);

    /* Prepare an Array of "remaining stores", and count them */
    for (n = 0; n < MAX_STORES; n++) rooms[n] = n;

    /* Place rows of stores */
    for (y = 0; y < n_rows; y++) {
		for (x = 0; x < n_cols; x++) {
			if (n < 1) break;

			/* Pick a remaining store */
			k = randint0(n);

			/* Build that store at the proper location */
			build_store(c, rooms[k], y, x);

			/* Shift the stores down, remove one store */
			rooms[k] = rooms[--n];
		}
    }

    /* Place the stairs */
    find_empty_range(c, &y, 3, TOWN_HGT - 3, &x, 3, TOWN_WID - 3);

    /* Clear previous contents, add down stairs */
    square_set_feat(c, y, x, FEAT_MORE);

    /* Place the player */
    player_place(c, p, y, x);
}


/**
 * Town logic flow for generation of new town.
 *
 * We start with a fully wiped cave of normal floors. This function does NOT do
 * anything about the owners of the stores, nor the contents thereof. It only
 * handles the physical layout.
 */
struct cave *town_gen(struct player *p) {
    int i, y, x;
    bool daytime = turn % (10 * TOWN_DAWN) < (10 * TOWN_DUSK);
    int residents = daytime ? MIN_M_ALLOC_TD : MIN_M_ALLOC_TN;
	struct cave *c;

	c = chunk_find_name("Town");

	/* First time */
	if (!c) {
		c = cave_new(TOWN_HGT, TOWN_WID);
		c->depth = p->depth;

		/* Build stuff */
		town_gen_layout(c, p);
	} else {

		/* Find the stairs (lame) */
		for (y = 0; y < c->height; y++) {
			bool found = FALSE;
			for (x = 0; x < c->width; x++) {
				if (c->feat[y][x] == FEAT_MORE) {
					found = TRUE;
					break;
				}
			}
			if (found) break;
		}

		/* Place the player */
		player_place(c, p, y, x);
	}

    /* Apply illumination */
    cave_illuminate(c, daytime);

    /* Make some residents */
    for (i = 0; i < residents; i++)
		pick_and_place_distant_monster(c, loc(p->px, p->py), 3, TRUE, c->depth);

    return c;
}


/* ------------------ MODIFIED ---------------- */

/**
 * Room profiles for moria levels - idea stolen from Oangband
 */
struct room_profile moria_rooms[] = {
	/* really big rooms have rarity 0 but they have other checks */
	{"greater vault", build_greater_vault, 44, 66, 35, FALSE, 0, 100},

	/* very rare rooms (rarity=2) */
	{"room of chambers", build_room_of_chambers, 44, 66, 10, FALSE, 2, 4},
	{"monster pit", build_pit, 11, 33, 5, TRUE, 2, 12},
	{"monster nest", build_nest, 11, 33, 5, TRUE, 2, 20},
	{"medium vault", build_medium_vault, 22, 33, 30, FALSE, 2, 40},
	{"lesser vault", build_lesser_vault, 22, 33, 20, FALSE, 2, 60},
	{"interesting room", build_interesting, 44, 55, 0, FALSE, 2, 100},

	/* normal rooms */
	{"simple room", build_moria, 11, 33, 1, FALSE, 0, 100}
};

/**
 * The main modified generation algorithm
 */
struct cave *modified_chunk(int depth, int height, int width)
{
    int i, y, x, y1, x1;
    int by = 0, bx = 0, key, rarity;
    int num_floors;
	int num_rooms = dun->profile->n_room_profiles;
    int dun_unusual = dun->profile->dun_unusual;
	bool moria_level = FALSE;

    /* Make the cave */
    struct cave *c = cave_new(height, width);
	c->depth = depth;

	/* Set the intended number of floor grids based on cave floor area */
    num_floors = c->height * c->width / 7;
    ROOM_LOG("height=%d  width=%d  nfloors=%d", c->height, c->width,num_floors);

    /* Fill cave area with basic granite */
    fill_rectangle(c, 0, 0, c->height - 1, c->width - 1, 
				   FEAT_GRANITE, SQUARE_NONE);

    /* Generate permanent walls around the generated area (temporarily!) */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, 
				   FEAT_PERM, SQUARE_NONE);

    /* Actual maximum number of blocks on this level */
    dun->row_blocks = c->height / dun->block_hgt;
    dun->col_blocks = c->width / dun->block_wid;

    /* Initialize the room table */
	dun->room_map = mem_zalloc(dun->row_blocks * sizeof(bool*));
	for (i = 0; i < dun->row_blocks; i++)
		dun->room_map[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* No rooms yet, pits or otherwise. */
    dun->pit_num = 0;
    dun->cent_n = 0;

	/* Hack -- It is possible for levels to be moria-style. */
	if ((c->depth >= 10) && (c->depth < 40) && one_in_(40)) {
		moria_level = TRUE;
		num_rooms = N_ELEMENTS(moria_rooms);
		ROOM_LOG("Moria level");
	}

    /* Build rooms until we have enough floor grids */
    while (c->feat_count[FEAT_FLOOR] < num_floors) {

		/* Roll for random key (to be compared against a profile's cutoff) */
		key = randint0(100);

		/* We generate a rarity number to figure out how exotic to make the
		 * room. This number has a depth/DUN_UNUSUAL chance of being > 0,
		 * a depth^2/DUN_UNUSUAL^2 chance of being > 1, up to MAX_RARITY. */
		i = 0;
		rarity = 0;
		while (i == rarity && i < dun->profile->max_rarity) {
			if (randint0(dun_unusual) < 50 + c->depth / 2) rarity++;
			i++;
		}

		/* Once we have a key and a rarity, we iterate through out list of
		 * room profiles looking for a match (whose cutoff > key and whose
		 * rarity > this rarity). We try building the room, and if it works
		 * then we are done with this iteration. We keep going until we find
		 * a room that we can build successfully or we exhaust the profiles. */
		for (i = 0; i < num_rooms; i++) {
			struct room_profile profile = moria_level ? moria_rooms[i] :
				dun->profile->room_profiles[i];
			if (profile.rarity > rarity) continue;
			if (profile.cutoff <= key) continue;
			if (room_build(c, by, bx, profile, TRUE)) break;
		}
    }

	for (i = 0; i < dun->row_blocks; i++)
		mem_free(dun->room_map[i]);
	mem_free(dun->room_map);

    /* Hack -- Scramble the room order */
    for (i = 0; i < dun->cent_n; i++) {
		int pick1 = randint0(dun->cent_n);
		int pick2 = randint0(dun->cent_n);
		y1 = dun->cent[pick1].y;
		x1 = dun->cent[pick1].x;
		dun->cent[pick1].y = dun->cent[pick2].y;
		dun->cent[pick1].x = dun->cent[pick2].x;
		dun->cent[pick2].y = y1;
		dun->cent[pick2].x = x1;
    }

    /* Start with no tunnel doors */
    dun->door_n = 0;

    /* Hack -- connect the first room to the last room */
    y = dun->cent[dun->cent_n-1].y;
    x = dun->cent[dun->cent_n-1].x;

    /* Connect all the rooms together */
    for (i = 0; i < dun->cent_n; i++) {
		/* Connect the room to the previous room */
		build_tunnel(c, dun->cent[i].y, dun->cent[i].x, y, x);

		/* Remember the "previous" room */
		y = dun->cent[i].y;
		x = dun->cent[i].x;
    }

    /* Place intersection doors */
    for (i = 0; i < dun->door_n; i++) {
		/* Extract junction location */
		y = dun->door[i].y;
		x = dun->door[i].x;

		/* Try placing doors */
		try_door(c, y, x - 1);
		try_door(c, y, x + 1);
		try_door(c, y - 1, x);
		try_door(c, y + 1, x);
    }

    ensure_connectedness(c);

    /* Turn the outer permanent walls back to granite  */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, 
				   FEAT_GRANITE, SQUARE_NONE);

	return c;
}

/**
 * Generate a new dungeon level.
 *
 * This is sample code to illustrate some of the new dungeon generation
 * methods; I think it actually prdouces quite nice levels.  New stuff:
 *
 * - different sized levels
 * - independence from block size: the block size can be set to any number
 *   from 1 (no blocks) to about 15; beyond that it struggles to generate
 *   enough floor space
 * - the find_space function, called from the room builder functions, allows
 *   the room to find space for itself rather than the generation algorithm
 *   allocating it; this helps because the room knows better what size it is
 * - a count is now kept of grids of the various terrains, allowing dungeon
 *   generation to terminate when enough floor is generated
 * - there are four new room types - moria rooms, huge rooms, rooms of
 *   chambers and interesting rooms - as well as many new vaults
 * - there is the ability to place specific monsters and objects in vaults and
 *   interesting rooms, as well as to make general monster restrictions in
 *   areas or the whole dungeon
 */
struct cave *modified_gen(struct player *p) {
    int i, k;
    int size_percent, y_size, x_size;
	bool moria_level = FALSE;
	struct cave *c;

    /* Scale the level */
    i = randint1(10) + p->depth / 24;
    if (is_quest(p->depth)) size_percent = 100;
    else if (i < 2) size_percent = 75;
    else if (i < 3) size_percent = 80;
    else if (i < 4) size_percent = 85;
    else if (i < 5) size_percent = 90;
    else if (i < 6) size_percent = 95;
    else size_percent = 100;
	y_size = DUNGEON_HGT * (size_percent - 5 + randint0(10)) / 100;
	x_size = DUNGEON_WID * (size_percent - 5 + randint0(10)) / 100;

    /* Set the block height and width */
	dun->block_hgt = dun->profile->block_size;
	dun->block_wid = dun->profile->block_size;

    c = modified_chunk(p->depth, MIN(DUNGEON_HGT, y_size),
					   MIN(DUNGEON_WID, x_size));
	c->depth = p->depth;

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1,
				   FEAT_PERM, SQUARE_NONE);

    /* Add some magma streamers */
    for (i = 0; i < dun->profile->str.mag; i++)
		build_streamer(c, FEAT_MAGMA, dun->profile->str.mc);

    /* Add some quartz streamers */
    for (i = 0; i < dun->profile->str.qua; i++)
		build_streamer(c, FEAT_QUARTZ, dun->profile->str.qc);

    /* Place 3 or 4 down stairs near some walls */
    alloc_stairs(c, FEAT_MORE, rand_range(3, 4), 3);

    /* Place 1 or 2 up stairs near some walls */
    alloc_stairs(c, FEAT_LESS, rand_range(1, 2), 3);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(c->depth / 3, 10), 2);

    /* Put some rubble in corridors */
    alloc_objects(c, SET_CORR, TYP_RUBBLE, randint1(k), c->depth, 0);

    /* Place some traps in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_TRAP, randint1(k), c->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Pick a base number of monsters */
    i = MIN_M_ALLOC_LEVEL + randint1(8) + k;

	/* Moria levels have a high proportion of cave dwellers. */
	if (moria_level) {
		/* Set global monster restriction variables. */
			mon_restrict("Moria dwellers", c->depth, TRUE);
	} else {
		/* Remove all monster restrictions. */
		mon_restrict(NULL, c->depth, TRUE);
	}

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
		pick_and_place_distant_monster(c, loc(p->px, p->py), 0, TRUE, c->depth);

    /* Put some objects in rooms */
    alloc_objects(c, SET_ROOM, TYP_OBJECT, Rand_normal(AMT_ROOM, 3),
				  c->depth, ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_OBJECT, Rand_normal(AMT_ITEM, 3),
				  c->depth, ORIGIN_FLOOR);
    alloc_objects(c, SET_BOTH, TYP_GOLD, Rand_normal(AMT_GOLD, 3),
				  c->depth, ORIGIN_FLOOR);

    return c;
}


/* ------------------ HARD CENTRE ---------------- */

struct cave *vault_chunk(struct player *p)
{
	struct vault *v;
	struct cave *c;

	if (one_in_(2)) v = random_vault(p->depth, 2);
	else v = random_vault(p->depth, 8);

	/* Make the chunk */
	c = cave_new(v->hgt, v->wid);
	c->depth = p->depth;

	/* Build the vault in it */
	build_vault(c, v->hgt / 2, v->wid / 2, v);

	return c;
}

/**
 * Make sure that all the caverns surrounding the centre are connected.
 */
void connect_caverns(struct cave *c, struct loc floor[])
{
	int i;
    int size = c->height * c->width;
    int *colors = mem_zalloc(size * sizeof(int));
    int *counts = mem_zalloc(size * sizeof(int));
	int color_of_floor[4];

	/* Color the regions, find which cavern os which color */
    build_colors(c, colors, counts, TRUE);
	for (i = 0; i < 4; i++) {
		int spot = yx_to_i(floor[i].y, floor[i].x, c->width);
		color_of_floor[i] = colors[spot];
	}

	/* Join left and upper, right and lower */
	join_region(c, colors, counts, color_of_floor[0], color_of_floor[1]);
	join_region(c, colors, counts, color_of_floor[2], color_of_floor[3]);

	/* Redo the colors, join the two big caverns */
    build_colors(c, colors, counts, TRUE);
	for (i = 1; i < 3; i++) {
		int spot = yx_to_i(floor[i].y, floor[i].x, c->width);
		color_of_floor[i] = colors[spot];
	}
	join_region(c, colors, counts, color_of_floor[1], color_of_floor[2]);

    mem_free(colors);
    mem_free(counts);
}

struct cave *hard_centre_gen(struct player *p)
{
	/* Make a vault for the centre */
	struct cave *centre = vault_chunk(p);
	int rotate = 0;

	/* Dimensions for the surrounding caverns */
	int centre_cavern_hgt;
	int centre_cavern_wid;
	struct cave *upper_cavern;
	struct cave *lower_cavern;
	int lower_cavern_ypos;
	int side_cavern_wid;
	struct cave *left_cavern;
	struct cave *right_cavern;
	struct cave *c;
	int i, k, y, x, cavern_area;
	struct loc floor[4];

	/* Measure the vault, rotate to make it wider than it is high */
	if (centre->height > centre->width) {
		rotate = 1;
		centre_cavern_hgt = (DUNGEON_HGT - centre->width) / 2;
		centre_cavern_wid = centre->height;
		lower_cavern_ypos = centre_cavern_hgt + centre->width;
	} else {
		centre_cavern_hgt = (DUNGEON_HGT - centre->height) / 2;
		centre_cavern_wid = centre->width;
		lower_cavern_ypos = centre_cavern_hgt + centre->height;
	}

	/* Make the caverns */
	upper_cavern = cavern_chunk(p->depth, centre_cavern_hgt, centre_cavern_wid);
	lower_cavern = cavern_chunk(p->depth, centre_cavern_hgt, centre_cavern_wid);
	side_cavern_wid = (DUNGEON_WID - centre_cavern_wid) / 2;
	left_cavern = cavern_chunk(p->depth, DUNGEON_HGT, side_cavern_wid);
	right_cavern = cavern_chunk(p->depth, DUNGEON_HGT, side_cavern_wid);

	/* Return on failure */
	if (!upper_cavern || !lower_cavern || !left_cavern || !right_cavern)
		return NULL;

	/* Make a cave to copy them into, and find a floor square in each cavern */
	c = cave_new(DUNGEON_HGT, DUNGEON_WID);
	c->depth = p->depth;

	/* Left */
	chunk_copy(c, left_cavern, 0, 0, 0, FALSE);
	find_empty_range(c, &y, 0, DUNGEON_HGT, &x, 0, side_cavern_wid);
	floor[0].y = y;
	floor[0].x = x;

	/* Upper */
	chunk_copy(c, upper_cavern, 0, side_cavern_wid, 0, FALSE);
	find_empty_range(c, &y, 0, centre_cavern_hgt, &x, side_cavern_wid,
					 side_cavern_wid + centre_cavern_wid);
	floor[1].y = y;
	floor[1].x = x;

	/* Centre */
	chunk_copy(c, centre, centre_cavern_hgt, side_cavern_wid, rotate, FALSE);

	/* Lower */
	chunk_copy(c, lower_cavern, lower_cavern_ypos, side_cavern_wid, 0, FALSE);
	find_empty_range(c, &y, lower_cavern_ypos, DUNGEON_HGT, &x,
					 side_cavern_wid, side_cavern_wid + centre_cavern_wid);
	floor[3].y = y;
	floor[3].x = x;

	/* Right */
	chunk_copy(c, right_cavern, 0, side_cavern_wid + centre_cavern_wid, 0, FALSE);
	find_empty_range(c, &y, 0, DUNGEON_HGT, &x,
					 side_cavern_wid + centre_cavern_wid, DUNGEON_WID);
	floor[2].y = y;
	floor[2].x = x;

	/* Free all the chunks */
	cave_free(left_cavern);
	cave_free(upper_cavern);
	cave_free(centre);
	cave_free(lower_cavern);
	cave_free(right_cavern);

	/* Encase in perma-rock */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1,
				   FEAT_PERM, SQUARE_NONE);

	/* Connect up all the caverns */
	connect_caverns(c, floor);

	/* Connect to the centre */
	ensure_connectedness(c);

	cavern_area = 2 * (side_cavern_wid * DUNGEON_HGT +
					   centre_cavern_wid * centre_cavern_hgt);

	/* Place 2-3 down stairs near some walls */
	alloc_stairs(c, FEAT_MORE, rand_range(1, 3), 3);

	/* Place 1-2 up stairs near some walls */
	alloc_stairs(c, FEAT_LESS, rand_range(1, 2), 3);

	/* Generate some rubble, traps and monsters */
	k = MAX(MIN(c->depth / 3, 10), 2);

	/* Scale number by total cavern size - caverns are fairly sparse */
	k = (k * cavern_area) / (DUNGEON_HGT * DUNGEON_WID);

	/* Put some rubble in corridors */
	alloc_objects(c, SET_BOTH, TYP_RUBBLE, randint1(k), c->depth, 0);

	/* Place some traps in the dungeon */
	alloc_objects(c, SET_BOTH, TYP_TRAP, randint1(k), c->depth, 0);

	/* Determine the character location */
	new_player_spot(c, p);

	/* Put some monsters in the dungeon */
	for (i = randint1(8) + k; i > 0; i--)
		pick_and_place_distant_monster(c, loc(p->px, p->py), 0, TRUE, c->depth);

	/* Put some objects/gold in the dungeon */
	alloc_objects(c, SET_BOTH, TYP_OBJECT, Rand_normal(k, 2), c->depth + 5,
				  ORIGIN_CAVERN);
	alloc_objects(c, SET_BOTH, TYP_GOLD, Rand_normal(k / 2, 2), c->depth,
				  ORIGIN_CAVERN);
	alloc_objects(c, SET_BOTH, TYP_GOOD, randint0(k / 4), c->depth,
				  ORIGIN_CAVERN);

	return c;
}


/* ------------------ LAIR ---------------- */

struct cave *lair_gen(struct player *p) {
    int i, k;
	struct cave *c;
	struct cave *normal;
	struct cave *lair;

    /* Set the block height and width */
	dun->block_hgt = dun->profile->block_size;
	dun->block_wid = dun->profile->block_size;

    normal = modified_chunk(p->depth, DUNGEON_HGT, DUNGEON_WID / 2);
	if (!normal) return NULL;
	normal->depth = p->depth;

	lair = cavern_chunk(p->depth, DUNGEON_HGT, DUNGEON_WID / 2);
	if (!lair) return NULL;
	lair->depth = p->depth;

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(p->depth / 3, 10), 2) / 2;

    /* Put the character in the normal half */
    new_player_spot(normal, p);

    /* Pick a smallish number of monsters for the normal half */
    i = randint1(4) + k;

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
		pick_and_place_distant_monster(normal, loc(p->px, p->py), 0, TRUE,
									   normal->depth);

    /* Add some magma streamers */
    for (i = 0; i < dun->profile->str.mag; i++)
		build_streamer(normal, FEAT_MAGMA, dun->profile->str.mc);

    /* Add some quartz streamers */
    for (i = 0; i < dun->profile->str.qua; i++)
		build_streamer(normal, FEAT_QUARTZ, dun->profile->str.qc);

    /* Pick a larger number of monsters for the lair */
    i = (MIN_M_ALLOC_LEVEL + randint1(6) + k);

	/* Find appropriate monsters */
	while (TRUE) {
		/* Choose a pit profile */
		set_pit_type(lair->depth, 0);

		/* Set monster generation restrictions */
		if (mon_restrict(dun->pit_type->name, lair->depth, TRUE))
			break;
	}

	ROOM_LOG("Monster lair - %s", dun->pit_type->name);

    /* Place lair monsters */
	spread_monsters(lair, dun->pit_type->name, lair->depth, i, lair->height / 2,
					lair->width / 2, lair->height / 2, lair->width / 2, 
					ORIGIN_CAVERN);

	/* Remove our restrictions. */
	(void) mon_restrict(NULL, lair->depth, FALSE);

	/* Make the level */
	c = cave_new(DUNGEON_HGT, DUNGEON_WID);
	c->depth = p->depth;
	if (one_in_(2)) {
		chunk_copy(c, lair, 0, 0, 0, FALSE);
		chunk_copy(c, normal, 0, DUNGEON_WID / 2, 0, FALSE);

		/* The player needs to move */
		p->px += DUNGEON_WID / 2;
	} else {
		chunk_copy(c, normal, 0, 0, 0, FALSE);
		chunk_copy(c, lair, 0, DUNGEON_WID / 2, 0, FALSE);
	}

	/* Free the chunks */
	cave_free(normal);
	cave_free(lair);

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, 
				   FEAT_PERM, SQUARE_NONE);

	/* Connect */
	ensure_connectedness(c);

    /* Place 3 or 4 down stairs near some walls */
    alloc_stairs(c, FEAT_MORE, rand_range(3, 4), 3);

    /* Place 1 or 2 up stairs near some walls */
    alloc_stairs(c, FEAT_LESS, rand_range(1, 2), 3);

    /* Put some rubble in corridors */
    alloc_objects(c, SET_CORR, TYP_RUBBLE, randint1(k), c->depth, 0);

    /* Place some traps in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_TRAP, randint1(k), c->depth, 0);

    /* Put some objects in rooms */
    alloc_objects(c, SET_ROOM, TYP_OBJECT, Rand_normal(AMT_ROOM, 3),
				  c->depth, ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(c, SET_BOTH, TYP_OBJECT, Rand_normal(AMT_ITEM, 3),
				  c->depth, ORIGIN_FLOOR);
    alloc_objects(c, SET_BOTH, TYP_GOLD, Rand_normal(AMT_GOLD, 3),
				  c->depth, ORIGIN_FLOOR);

    return c;
}
