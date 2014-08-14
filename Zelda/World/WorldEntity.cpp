#include "WorldEntity.hpp"
#include <OgreSceneNode.h>
#include <BulletCollision/CollisionDispatch/btCollisionObject.h>
#include "../Common/Physics/BtOgreExtras.h"
#include "../Common/Util/DeleteSceneNode.hpp"
#include "Atlas/Map.hpp"

CWorldEntity::CWorldEntity(const std::string &sID, CEntity *pParent, CMap *pMap)
  : CEntity(sID, pParent),
    m_pSceneNode(nullptr),
    m_pCollisionObject(nullptr),
    m_pMap(pMap) {
}
CWorldEntity::~CWorldEntity() {
}

void CWorldEntity::exit() {
  if (m_pSceneNode) {
    destroySceneNode(m_pSceneNode, true);
    m_pSceneNode = nullptr;
  }
  if (m_pCollisionObject) {
    assert(m_pMap);
  	m_pMap->getPhysicsManager()->getWorld()->removeCollisionObject(m_pCollisionObject);
    btRigidBody *pRB = btRigidBody::upcast(m_pCollisionObject);
    if (pRB) {
      delete pRB->getMotionState();
    }
    // dont delete collision shape, since it is normally a global shape
    m_pCollisionObject = nullptr;
  }

  CEntity::exit();
}

const SPATIAL_VECTOR &CWorldEntity::getPosition() const {
  return m_pSceneNode->getPosition();
}
void CWorldEntity::setPosition(const SPATIAL_VECTOR &vPos) {
  if (m_pCollisionObject) {
    m_pCollisionObject->getWorldTransform().setOrigin(BtOgre::Convert::toBullet(vPos));
  }
  m_pSceneNode->setPosition(vPos);
}
void CWorldEntity::translate(const SPATIAL_VECTOR &vOffset) {
  m_pSceneNode->translate(vOffset);
}

const SPATIAL_VECTOR &CWorldEntity::getCenter() const {
  return getPosition();
}
void CWorldEntity::setCenter(const SPATIAL_VECTOR &vCenter) {
  setPosition(vCenter);
}

const SPATIAL_VECTOR &CWorldEntity::getSize() const {
  return getScale();
}
void CWorldEntity::setSize(const SPATIAL_VECTOR &vSize) {
  setScale(vSize);
}

const SPATIAL_VECTOR &CWorldEntity::getScale() const {
  return m_pSceneNode->getScale();
}
void CWorldEntity::setScale(const SPATIAL_VECTOR &vScale) {
  m_pSceneNode->setScale(vScale);
}

const Ogre::Quaternion &CWorldEntity::getOrientation() const {
  return m_pSceneNode->getOrientation();
}
void CWorldEntity::setOrientation(const Ogre::Quaternion &quat) {
  m_pSceneNode->setOrientation(quat);
}

Ogre::SceneNode *CWorldEntity::getSceneNode() const {
  return m_pSceneNode;
}
btCollisionObject *CWorldEntity::getCollisionObject() const {
  return m_pCollisionObject;
}

void CWorldEntity::update(Ogre::Real tpf) {
  CHitableInterface::update(tpf);
  CEntity::update(tpf);
}

void CWorldEntity::setThisAsCollisionObjectsUserPointer() {
  assert(m_pCollisionObject);
  m_pCollisionObject->setUserPointer(this);
}

CWorldEntity *CWorldEntity::getFromUserPointer(btCollisionObject *pCO) {
  assert(pCO);
  return static_cast<CWorldEntity*>(pCO->getUserPointer());
}

CWorldEntity *CWorldEntity::getFromUserPointer(const btCollisionObject *pCO) {
  assert(pCO);
  return static_cast<CWorldEntity*>(pCO->getUserPointer());
}
