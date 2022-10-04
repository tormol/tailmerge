/* This file is part of tailmerge.
 * Copyright (C) 2022 Torbjørn Birch Moltu
 *
 * Licenced under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * tailmerge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with tailmerge. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
/* Helper functions */

/// Print error message based on errno and then exit if `ret` is negative,
/// Otherwise pass it through to caller.
///
/// The error message is "Failed to " + desc including var-args + ": " + strerror(errno) + "\n"
int checkerr(int ret, int exit_status, const char* desc, ...);

#ifdef __linux__
/// Similar to checkerr(), but instead of errno, -ret is passed to strerror().
int checkerr_sys(int ret, int exit_status, const char* desc, ...);
#endif
