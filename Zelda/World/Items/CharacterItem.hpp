/*****************************************************************************
 * Copyright 2014 Christoph Wick
 *
 * This file is part of Zelda.
 *
 * Zelda is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Zelda is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Zelda. If not, see http://www.gnu.org/licenses/.
 *****************************************************************************/

#ifndef _CHARACTER_ITEM_HPP_
#define _CHARACTER_ITEM_HPP_

#include "ItemTypes.hpp"
#include <OgrePrerequisites.h>
#include "../Damage.hpp"

class CCharacter;
class btRigidBody;
class CMap;

class CCharacterItem {
protected:
  CCharacter *mCharacter;
  const EItemVariantTypes mVariantType;
  Ogre::Entity *mAttachedMesh;
  const std::string mBoneToAttach;

  btRigidBody *mBlockPhysics;

  Ogre::Vector3 mOldDamageStartPos;
public:
  CCharacterItem(CCharacter *character, const std::string &boneToAttach, EItemVariantTypes type);
  ~CCharacterItem();

  void enterNewMap(CMap *oldMap, CMap *newMap);
  void update(Ogre::Real tpf);
  
  void startDamage();
  void updateDamage(Ogre::Real tpf);

  EItemVariantTypes getItemVariantType() const {return mVariantType;}

  void show();
  void hide();

  virtual CDamage createDamage();
  virtual Ogre::Vector3 getDamagePosition();

 private:
  void createPhysics(CMap *map);
  void destroyPhysics(CMap *map);
};

#endif // _CHARACTER_ITEM_HPP_
