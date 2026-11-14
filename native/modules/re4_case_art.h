// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
namespace re4 {
inline std::array<unsigned,2> CaseSize(unsigned id) {
 switch(id) {
 case 3:return {5,2};
 case 33:return {3,2};
 case 64:return {3,2};
 case 35:return {3,2};
 case 37:return {4,2};
 case 39:return {3,2};
 case 41:return {4,2};
 case 42:return {4,2};
 case 44:return {8,2};
 case 45:return {5,2};
 case 46:return {9,1};
 case 47:return {7,2};
 case 48:return {3,2};
 case 52:return {7,3};
 case 53:return {8,2};
 case 54:return {5,2};
 case 55:return {4,2};
 case 56:return {1,3};
 case 148:return {8,2};
 case 23:return {8,2};
 case 109:return {8,2};
 case 62:return {4,2};
 case 82:return {7,3};
 case 4:return {2,1};
 case 32:return {2,1};
 case 24:return {2,1};
 case 7:return {2,1};
 case 106:return {2,1};
 case 26:return {2,1};
 case 0:return {2,1};
 case 70:return {2,1};
 case 114:return {4,1};
 case 63:return {2,1};
 case 66:return {3,1};
 case 67:return {2,2};
 case 68:return {3,1};
 case 69:return {3,1};
 case 170:return {2,2};
 case 197:return {3,1};
 case 1:return {1,2};
 case 2:return {1,2};
 case 14:return {1,2};
 case 5:return {1,2};
 case 8:return {1,1};
 case 9:return {1,1};
 case 10:return {1,1};
 case 149:return {1,3};
 case 151:return {2,6};
 case 6:return {1,2};
 case 25:return {1,2};
 case 28:return {1,2};
 case 20:return {1,2};
 case 22:return {1,2};
 case 168:return {1,2};
 case 18:return {1,2};
 case 19:return {1,2};
 case 21:return {1,2};
 case 12:return {1,2};
 case 65:return {7,3};
 case 16:return {5,3};
 case 17:return {4,1};
 case 71:return {6,2};
 case 83:return {7,3};
 case 34:return {2,2};
 default:return {0,0};
 }
}
inline bool CaseArtSupported(unsigned id){return CaseSize(id)[0]!=0;}
}
