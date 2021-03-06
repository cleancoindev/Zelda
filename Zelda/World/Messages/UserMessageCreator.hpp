#ifndef _USER_MESSAGE_CREATOR_HPP_
#define _USER_MESSAGE_CREATOR_HPP_

#include "../../Common/Message/MessageCreator.hpp"

class CUserMessageCreator : public CMessageCreator {
public:
  virtual CMessage *createMessage(const tinyxml2::XMLElement *pElem, const Ogre::Any &any = Ogre::Any());
};

#endif // _USER_MESSAGE_CREATOR_HPP_
