/*
 * Copyright (c) 2020 Bryan Cain
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

// returns a newly allocated string; it's the caller's responsibility to free it
// example: string_replace_extension("/path/to/gamename.pce", ".sav") -> "/path/to/gamename.sav"
char * string_replace_extension(const char *original, const char *extension)
{
    char *dot_pos = strrchr(original, '.'), *result;
    size_t orig_len, ext_len = strlen(extension);
    if (!dot_pos || strchr(dot_pos, '/') || strchr(dot_pos, '\\'))
    {
        // file has no extension
        orig_len = strlen(original);
    }
    else
    {
        // file has an extension that starts with dot_pos
        orig_len = dot_pos - original;
    }
    result = malloc(orig_len + ext_len + 1);
    memcpy(result, original, orig_len);
    memcpy(result + orig_len, extension, ext_len);
    result[orig_len + ext_len] = 0;
    return result;
}

