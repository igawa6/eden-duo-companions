// SPDX-License-Identifier: GPL-3.0-or-later
// Factual Mega-form classification derived from PKHeX FormInfo.cs (GPL-3.0).
// https://github.com/kwsch/PKHeX/blob/master/PKHeX.Core/Legality/Tables/FormInfo.cs
#pragma once
#include <algorithm>
namespace za {
inline bool IsMegaForm(int species,int form){
 constexpr int species_ids[]={3,6,9,65,94,115,127,130,142,150,181,212,214,229,248,257,282,303,306,308,310,354,359,381,380,445,448,460,15,18,80,208,254,260,302,319,323,334,362,373,376,384,428,475,531,719,36,71,121,149,154,160,227,478,500,530,545,560,604,609,652,655,658,668,687,689,691,701,718,780,870,670,485,491,807,26,801,978,678,358,398,623,740,768,952,998,970};
 if(std::find(std::begin(species_ids),std::end(species_ids),species)==std::end(species_ids))return false;
 switch(species){case 25:return form==2||form==3;case 80:return form==1;case 718:return form==5;case 670:return form==6;case 658:return form==3;case 678:case 801:return form==2||form==3;case 978:return form>=3&&form<=5;default:return form>0;}
}
}
