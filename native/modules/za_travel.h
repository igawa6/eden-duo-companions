// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
namespace za {
class TravelController {
public:
 enum class Command {None,OpenMap,Destinations,Up,Down,Confirm};
 void Start(int destination,std::uint64_t tick){target_=destination;next_=tick;deadline_=tick+2400;previous_=0;direction_=0;unchanged_=0;stable_=0;primed_=false;confirming_=false;status_="Ouverture du voyage rapide...";}
 void Cancel(){if(Active())status_="Voyage annulé";target_=0;}
 bool Active()const{return target_>0;}
 const std::string& Status()const{return status_;}
 Command Poll(std::uint64_t tick,int mode,int destination){
  if(!Active()||tick<next_)return Command::None;
  if(tick>deadline_){Stop("Carte indisponible : fermez les menus et réessayez.");return Command::None;}
  if(confirming_){
   if(mode==4&&destination==target_){Stop("Voyage demandé au jeu");return Command::Confirm;}
   if(mode==3){Stop("Voyage demandé au jeu");return Command::None;}
   Stop("Confirmation du voyage indisponible : réessayez depuis la carte.");return Command::None;
  }
  // Before its first opening, the hidden map exposes the same cached mode as the plan.
  // Prime Plus once; an already open plan closes, is observed closed, then opens again.
  if(!primed_&&(mode==0||mode==3||mode==4)){primed_=true;next_=tick+90;return Command::OpenMap;}
  if(mode==4){Stop("Fermez la confirmation de la carte avant de réessayer.");return Command::None;}
  if(mode==2)primed_=true;
  if(mode==3){next_=tick+90;return Command::OpenMap;}
  if(mode==0){next_=tick+60;return Command::Destinations;}
  if(mode!=2)return Command::None;
  // The initial highlighted entry may not publish its destination until it changes.
  if(destination<=0){next_=tick+24;return Command::Down;}
  if(destination==target_){
   if(!stable_){stable_=tick+12;return Command::None;}
   if(tick<stable_)return Command::None;
   confirming_=true;next_=tick+60;status_="Confirmation du voyage...";return Command::Confirm;
  }
  stable_=0;
  // Stops on a missing locked destination, list wrap, or an unexpected direction change.
  if(previous_&&((direction_>0&&(destination<previous_||destination>target_))||
                (direction_<0&&(destination>previous_||destination<target_)))){
   Stop("Ce centre n'est pas proposé parmi les destinations débloquées.");return Command::None;}
  if(previous_==destination&&++unchanged_>=3){Stop("Navigation interrompue : réessayez depuis la carte du jeu.");return Command::None;}
  if(previous_!=destination)unchanged_=0;
  previous_=destination;direction_=destination<target_?1:-1;next_=tick+24;
  status_="Sélection du centre Pokémon...";
  return direction_>0?Command::Down:Command::Up;
 }
private:
 void Stop(const char* message){target_=0;status_=message;}
 bool primed_=false,confirming_=false;
 int target_=0,previous_=0,direction_=0,unchanged_=0;
 std::uint64_t next_=0,deadline_=0,stable_=0;
 std::string status_;
};
}
