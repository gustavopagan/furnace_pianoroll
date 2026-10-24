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

// piano roll window (fork piano-roll)

#include "gui.h"

void FurnaceGUI::drawPianoRoll() {
  if (!pianoRollOpen) return;
  ImGui::SetNextWindowSize(ImVec2(800.0f*dpiScale,500.0f*dpiScale),ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Piano Roll",&pianoRollOpen,globalWinFlags,_("Piano Roll"))) {
    ImGui::TextUnformatted(_("piano roll (empty window, stage E1)"));
  }
  ImGui::End();
}
