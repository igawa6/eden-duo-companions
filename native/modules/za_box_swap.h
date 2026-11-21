// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "za_live.h"
#include <algorithm>
#include <string>
namespace za {
// All movement goes through the game's own Y/A/B controls. No storage writes.
// In particular, A is unreachable until the drag record has followed a nudge:
// this distinguishes an actual pickup from an ignored Y, even for box 0/slot 0.
class BoxSwapController {
public:
 enum class Command {None,Left,Right,Up,Down,Pick,Confirm,Back,NextBox,PreviousBox};
 void Start(int box,int slot,int party,const Entity& source,const Entity& victim,std::uint64_t tick){
  box_=box;slot_=slot;party_=party;source_=Token(source);victim_=Token(victim);
  phase_=Focus;next_=tick;deadline_=tick+4800;attempts_=0;cancel_=false;completed_=false;pickup_issued_=false;
  status_="Vérification du curseur des boîtes...";
 }
 void Cancel(){cancel_=true;}
 bool Active()const{return phase_!=Idle;}
 bool Completed()const{return completed_;}
 const std::string& Status()const{return status_;}
 Command Poll(std::uint64_t tick,const LiveSnapshot& live){
  if(!Active()||tick<next_)return Command::None;
  const auto& s=live.boxes;
  if(cancel_){Stop("Échange annulé");return (pickup_issued_||(s.valid&&!s.EmptyDrag()))?Command::Back:Command::None;}
  if(tick>deadline_){Stop("Échange interrompu : vérifiez les boîtes dans le jeu.");return (pickup_issued_||(s.valid&&!s.EmptyDrag()))?Command::Back:Command::None;}
  if(!s.valid){status_="Recherche du curseur : laissez les boîtes du jeu ouvertes.";return Command::None;}
  if(phase_<=PickSource&&(!live.party_valid||Token(live.party[party_])!=victim_)){
   Stop("L'équipe a changé. Sélectionnez à nouveau les Pokémon.");return Command::None;
  }
  auto pulse=[&](Command command,int delay=22){next_=tick+delay;return command;};
  auto move=[&](int from,int to){
   if(from/6>to/6)return Command::Up;if(from/6<to/6)return Command::Down;
   return from%6>to%6?Command::Left:Command::Right;
  };
  auto failed=[&](const char* message){Stop(message);return pickup_issued_||!s.EmptyDrag()?Command::Back:Command::None;};
  switch(phase_){
  case Focus:
   if(!s.EmptyDrag())return failed("Posez d'abord le Pokémon tenu dans le jeu.");
   nudge_=s.cursor+(s.cursor%6==5?-1:1);phase_=FocusWait;
   return pulse(nudge_>s.cursor?Command::Right:Command::Left);
  case FocusWait:
   if(s.cursor!=nudge_||!s.EmptyDrag())return failed("Ouvrez les boîtes du jeu et fermez leurs autres menus.");
   phase_=Anchor;return Command::None;
  case Anchor:
   // A first-row index also exists in the party. Prove focus on a box row >= 1.
   if(s.cursor<6){expected_=s.cursor+6;phase_=AnchorWait;return pulse(Command::Down);}
   phase_=SelectSource;return Command::None;
  case AnchorWait:
   if(s.cursor<6||!s.EmptyDrag())return failed("Le curseur des boîtes ne répond pas. Réessayez.");
   phase_=SelectSource;return Command::None;
  case SelectSource:
   if(!s.EmptyDrag())return failed("Déplacement manuel détecté : échange arrêté.");
   if(s.cursor!=slot_){expected_=s.cursor;return pulse(move(s.cursor,slot_));}
   if(!live.box_valid||live.box_view_index!=box_||Token(live.box_view[slot_])!=source_)
    return failed("La boîte a changé : actualisez la sélection avant l'échange.");
   pickup_issued_=true;phase_=AfterPick;status_="Prise du Pokémon dans les boîtes...";return pulse(Command::Pick);
  case AfterPick:
   nudge_=slot_+(slot_%6==5?-1:1);phase_=CheckPickup;
   return pulse(nudge_>slot_?Command::Right:Command::Left);
  case CheckPickup:
   if(s.cursor!=nudge_||s.source_slot!=slot_||s.target_slot!=nudge_||s.source_box!=s.target_box||s.EmptyDrag()){
    // A missing drag confirmation must cancel, never issue another Y on an uncertain pickup.
    return failed("La prise n'est pas confirmée. Aucun échange effectué.");
   }
   if(s.source_box!=box_){
    if(s.source_box<0||s.source_box>=32||++attempts_>=32)return failed("Boîte source introuvable.");
    pickup_issued_=false;box_steps_=box_-s.source_box;phase_=WrongBoxCancelled;return pulse(Command::Back);
   }
   phase_=ToParty;status_="Sélection du Pokémon de l'équipe...";return Command::None;
  case WrongBoxCancelled:
   if(!s.EmptyDrag())return failed("Annulation de la prise non confirmée.");
   phase_=ChangeBox;return Command::None;
  case ChangeBox:
   if(box_steps_){const bool forward=box_steps_>0;box_steps_+=forward?-1:1;return pulse(forward?Command::NextBox:Command::PreviousBox,40);}
   phase_=Anchor;return Command::None;
  case ToParty:
   if(s.target_box==box_){return pulse(Command::Up);}
   if(s.target_box!=998||s.source_box!=box_||s.source_slot!=slot_)return failed("Destination de déplacement inattendue.");
   if(s.target_slot!=party_)return pulse(s.target_slot>party_?Command::Left:Command::Right);
   if(s.party_cursor!=party_)return failed("Sélection d'équipe non confirmée.");
   phase_=ConfirmParty;next_=tick+12;return Command::None;
  case ConfirmParty:
   if(s.target_box!=998||s.target_slot!=party_||s.source_box!=box_||s.source_slot!=slot_||!live.party_valid||Token(live.party[party_])!=victim_)
    return failed("La destination a changé : échange arrêté.");
   phase_=AfterParty;status_="Échange dans l'équipe...";return pulse(Command::Confirm,40);
  case AfterParty:
   if(s.EmptyDrag()){phase_=Verify;verify_deadline_=tick+300;return Command::None;}
   phase_=ToBox;return Command::None;
  case ToBox:
   if(s.target_box==998)return pulse(Command::Down);
   if(s.target_box!=box_)return failed("Revenez à la boîte source pour terminer le déplacement.");
   if(s.target_slot!=slot_)return pulse(move(s.target_slot,slot_));
   phase_=ConfirmBox;next_=tick+12;return Command::None;
  case ConfirmBox:
   if(s.target_box!=box_||s.target_slot!=slot_||s.EmptyDrag())return failed("Cellule de retour non confirmée.");
   phase_=Verify;verify_deadline_=tick+300;status_="Vérification de l'échange...";return pulse(Command::Confirm,40);
  case Verify:
   if(s.EmptyDrag()&&live.party_valid&&Token(live.party[party_])==source_&&live.box_valid&&live.box_view_index==box_&&Token(live.box_view[slot_])==victim_){
    completed_=true;Stop("Échange confirmé dans le jeu");return Command::None;
   }
   if(tick>verify_deadline_)return failed("Déplacement terminé : sauvegardez puis actualisez pour vérifier le résultat.");
   return Command::None;
  default:return Command::None;
  }
 }
private:
 static std::uint64_t Token(const Entity& e){return (std::uint64_t(e.pid)<<32)|e.encryption_constant;}
 void Stop(const char* message){phase_=Idle;status_=message;}
 enum Phase {Idle,Focus,FocusWait,Anchor,AnchorWait,SelectSource,PickSource,AfterPick,CheckPickup,WrongBoxCancelled,ChangeBox,ToParty,ConfirmParty,AfterParty,ToBox,ConfirmBox,Verify};
 Phase phase_=Idle;
 int box_=0,slot_=0,party_=0,nudge_=0,expected_=0,box_steps_=0,attempts_=0;
 std::uint64_t source_=0,victim_=0,next_=0,deadline_=0,verify_deadline_=0;
 bool cancel_=false,completed_=false,pickup_issued_=false;
 std::string status_;
};
}
