// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace echoes {
// At most three complete fresh observations in this call. Never substitute a
// retained earlier snapshot when all attempts fail their original guards.
template<class Observe,class Qualified>
auto RequalifyFresh(Observe observe,Qualified qualified) {
  auto value=observe();
  for(unsigned attempt=1;attempt<3 && !qualified(value);++attempt)value=observe();
  return value;
}
} // namespace echoes
