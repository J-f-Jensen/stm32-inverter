/*
 * This file is part of the tumanako_vc project.
 *
 * Copyright (C) 2012 Johannes Huebner <contact@johanneshuebner.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef INV_CONTROL_H
#define INV_CONTROL_H

#include "my_fp.h"
#include "errormessage.h"

class inv_control
{
   public:
	static int RunInvControl();
	static void ResetInvControl();

   private:
	static void CruiseControl();
	static void SelectDirection();
	static int GetUserThrottleCommand();
	static void GetCruiseCreepCommand(s32fp& finalSpnt, s32fp throtSpnt);
	static s32fp ProcessThrottle();
    static void PostErrorIfRunning(ERROR_MESSAGE_NUM err);
};

#endif // THROTTLE_H
