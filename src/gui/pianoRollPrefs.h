/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2025 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// piano roll per-channel preferences (plan 3.5), stored next to the song as
// <song>.fur.roll.json so the .fur format is never touched.
// no ImGui, no engine: testable on its own.
//
// format (version 1):
// {
//   "version": 1,
//   "channels": {
//     "0": { "fimPadrao": "OFF", "fold": false, "nomesDeLinha": { "60": "kick" } }
//   }
// }

#ifndef _PIANO_ROLL_PREFS_H
#define _PIANO_ROLL_PREFS_H

#include <map>
#include <string>

struct PRChanPrefs {
  bool endRel;   // fimPadrao: false = OFF, true = REL
  bool fold;
  std::map<int,std::string> rowNames; // nomesDeLinha: pitch 0..179 -> text
  PRChanPrefs(): endRel(false), fold(false) {}
  bool isDefault() const { return !endRel && !fold && rowNames.empty(); }
};

struct PRPrefs {
  std::map<int,PRChanPrefs> chans;
  PRChanPrefs& get(int ch) { return chans[ch]; }
  std::string toJson() const;
  // returns false (and leaves *this untouched) if the text is not valid
  bool fromJson(const std::string& text);
};

// file helpers. path is the song path; ".roll.json" is appended.
bool prPrefsLoad(PRPrefs& prefs, const std::string& songPath);
bool prPrefsSave(const PRPrefs& prefs, const std::string& songPath);
std::string prPrefsPath(const std::string& songPath);

#endif
