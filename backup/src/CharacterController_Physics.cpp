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

#include "StdAfx.h"
#include "CharacterController_Physics.h"
#include "PhysicsManager.h"

const btScalar BORDER_CCD_PENETRATION_SPEED_ADD_FACTOR = 1.2;
const btScalar BORDER_CCD_PENETRATION_SPEED = 5;

// static helper method
static btVector3
getNormalizedVector(const btVector3& v)
{
	btVector3 n = v.normalized();
	if (n.length() < SIMD_EPSILON) {
		n.setValue(0, 0, 0);
	}
	return n;
}


///@todo Interact with dynamic objects,
///Ride kinematicly animated platforms properly
///More realistic (or maybe just a config option) falling
/// -> Should integrate falling velocity manually and use that in stepDown()
///Support jumping
///Support ducking
class btKinematicClosestNotMeRayResultCallback : public btCollisionWorld::ClosestRayResultCallback
{
public:
	btKinematicClosestNotMeRayResultCallback (btCollisionObject* me) : btCollisionWorld::ClosestRayResultCallback(btVector3(0.0, 0.0, 0.0), btVector3(0.0, 0.0, 0.0))
	{
		m_me = me;
	}

	virtual btScalar addSingleResult(btCollisionWorld::LocalRayResult& rayResult,bool normalInWorldSpace)
	{
		if (rayResult.m_collisionObject == m_me)
			return 1.0;

		return ClosestRayResultCallback::addSingleResult (rayResult, normalInWorldSpace);
	}
protected:
	btCollisionObject* m_me;
};

class btKinematicClosestNotMeConvexResultCallback : public btCollisionWorld::ClosestConvexResultCallback
{
public:
	btKinematicClosestNotMeConvexResultCallback (btCollisionObject* me, const btVector3& up, btScalar minSlopeDot)
	: btCollisionWorld::ClosestConvexResultCallback(btVector3(0.0, 0.0, 0.0), btVector3(0.0, 0.0, 0.0))
	, m_me(me)
	, m_up(up)
	, m_minSlopeDot(minSlopeDot)
	{
	}

	virtual btScalar addSingleResult(btCollisionWorld::LocalConvexResult& convexResult,bool normalInWorldSpace)
	{
		if (convexResult.m_hitCollisionObject == m_me)
			return btScalar(1.0);

		btVector3 hitNormalWorld;
		if (normalInWorldSpace)
		{
			hitNormalWorld = convexResult.m_hitNormalLocal;
		} else
		{
			///need to transform normal into worldspace
			hitNormalWorld = convexResult.m_hitCollisionObject->getWorldTransform().getBasis()*convexResult.m_hitNormalLocal;
		}

		btScalar dotUp = m_up.dot(hitNormalWorld);
		if (dotUp < m_minSlopeDot) {
			return btScalar(1.0);
		}

		return ClosestConvexResultCallback::addSingleResult (convexResult, normalInWorldSpace);
	}
protected:
	btCollisionObject* m_me;
	const btVector3 m_up;
	btScalar m_minSlopeDot;
};

/*
 * Returns the reflection direction of a ray going 'direction' hitting a surface with normal 'normal'
 *
 * from: http://www-cs-students.stanford.edu/~adityagp/final/node3.html
 */
btVector3 CharacterControllerPhysics::computeReflectionDirection (const btVector3& direction, const btVector3& normal)
{
	return direction - (btScalar(2.0) * direction.dot(normal)) * normal;
}

/*
 * Returns the portion of 'direction' that is parallel to 'normal'
 */
btVector3 CharacterControllerPhysics::parallelComponent (const btVector3& direction, const btVector3& normal)
{
	btScalar magnitude = direction.dot(normal);
	return normal * magnitude;
}

/*
 * Returns the portion of 'direction' that is perpindicular to 'normal'
 */
btVector3 CharacterControllerPhysics::perpindicularComponent (const btVector3& direction, const btVector3& normal)
{
	return direction - parallelComponent(direction, normal);
}

CharacterControllerPhysics::CharacterControllerPhysics (btPairCachingGhostObject* ghostObject,btConvexShape* convexShape,btConvexShape *pCSForBorderDetection, btScalar stepHeight, int upAxis)
{
	m_upAxis = upAxis;
	m_addedMargin = 0.02;
	m_walkDirection.setValue(0,0,0);
	m_useGhostObjectSweepTest = true;
	m_ghostObject = ghostObject;
	m_stepHeight = stepHeight;
	m_turnAngle = btScalar(0.0);
	m_convexShape=convexShape;
	m_convexShapeForBorderDetection=pCSForBorderDetection;
	m_useWalkDirection = true;	// use walk direction by default, legacy behavior
	m_velocityTimeInterval = 0.0;
	m_verticalVelocity = 0.0;
	m_verticalOffset = 0.0;
	m_gravity = GRAVITY_FACTOR ;
	m_fallSpeed = 55.0; // Terminal velocity of a sky diver in m/s.
	m_jumpSpeed = 0.5; // ?
	m_wasOnGround = false;
	m_wasJumping = false;
	setMaxSlope(btRadians(45.0));
	m_vBorderDetectionShapeOffset = btVector3(0, 0, 0);
	m_fCurrentBorderCcdPenetration = 0;

	full_drop = false;
	bounce_fix = false;
}

CharacterControllerPhysics::~CharacterControllerPhysics ()
{
}
void CharacterControllerPhysics::start() {
    m_BorderDetectionShapeGroup = getGhostObject()->getBroadphaseHandle()->m_collisionFilterGroup;
    m_BorderDetectionShapeMask = getGhostObject()->getBroadphaseHandle()->m_collisionFilterMask;
}


btPairCachingGhostObject* CharacterControllerPhysics::getGhostObject()
{
	return m_ghostObject;
}

bool CharacterControllerPhysics::recoverFromPenetration ( btCollisionWorld* collisionWorld)
{
	// Here we must refresh the overlapping paircache as the penetrating movement itself or the
	// previous recovery iteration might have used setWorldTransform and pushed us into an object
	// that is not in the previous cache contents from the last timestep, as will happen if we
	// are pushed into a new AABB overlap. Unhandled this means the next convex sweep gets stuck.
	//
	// Do this by calling the broadphase's setAabb with the moved AABB, this will update the broadphase
	// paircache and the ghostobject's internal paircache at the same time.    /BW

	btVector3 minAabb, maxAabb;
	m_convexShape->getAabb(m_ghostObject->getWorldTransform(), minAabb,maxAabb);
	collisionWorld->getBroadphase()->setAabb(m_ghostObject->getBroadphaseHandle(),
						 minAabb,
						 maxAabb,
						 collisionWorld->getDispatcher());

	bool penetration = false;

	collisionWorld->getDispatcher()->dispatchAllCollisionPairs(m_ghostObject->getOverlappingPairCache(), collisionWorld->getDispatchInfo(), collisionWorld->getDispatcher());

	m_currentPosition = m_ghostObject->getWorldTransform().getOrigin();

	btScalar maxPen = btScalar(0.0);
	for (int i = 0; i < m_ghostObject->getOverlappingPairCache()->getNumOverlappingPairs(); i++)
	{
		m_manifoldArray.resize(0);

		btBroadphasePair* collisionPair = &m_ghostObject->getOverlappingPairCache()->getOverlappingPairArray()[i];

		if (collisionPair->m_algorithm)
			collisionPair->m_algorithm->getAllContactManifolds(m_manifoldArray);


		for (int j=0;j<m_manifoldArray.size();j++)
		{
			btPersistentManifold* manifold = m_manifoldArray[j];
			btScalar directionSign = manifold->getBody0() == m_ghostObject ? btScalar(-1.0) : btScalar(1.0);
			for (int p=0;p<manifold->getNumContacts();p++)
			{
				const btManifoldPoint&pt = manifold->getContactPoint(p);

				btScalar dist = pt.getDistance();

				if (dist < 0.0)
				{
					if (dist < maxPen)
					{
						maxPen = dist;
						m_touchingNormal = pt.m_normalWorldOnB * directionSign;//??

					}
					m_currentPosition += pt.m_normalWorldOnB * directionSign * dist * btScalar(0.2);
					penetration = true;
				} else {
					//printf("touching %f\n", dist);
				}
			}

			//manifold->clearManifold();
		}
	}
	btTransform newTrans = m_ghostObject->getWorldTransform();
	newTrans.setOrigin(m_currentPosition);
	m_ghostObject->setWorldTransform(newTrans);
//	printf("m_touchingNormal = %f,%f,%f\n",m_touchingNormal[0],m_touchingNormal[1],m_touchingNormal[2]);
	return penetration;
}

void CharacterControllerPhysics::stepUp ( btCollisionWorld* world)
{
	// phase 1: up
	btTransform start, end;
	m_targetPosition = m_currentPosition + getUpAxisDirections()[m_upAxis] * (m_stepHeight + (m_verticalOffset > 0.f?m_verticalOffset:0.f));

	start.setIdentity ();
	end.setIdentity ();

	/* FIXME: Handle penetration properly */
	start.setOrigin (m_currentPosition + getUpAxisDirections()[m_upAxis] * (m_convexShape->getMargin() + m_addedMargin));
	end.setOrigin (m_targetPosition);

	btKinematicClosestNotMeConvexResultCallback callback (m_ghostObject, -getUpAxisDirections()[m_upAxis], btScalar(0.7071));
	callback.m_collisionFilterGroup = getGhostObject()->getBroadphaseHandle()->m_collisionFilterGroup;
	callback.m_collisionFilterMask = getGhostObject()->getBroadphaseHandle()->m_collisionFilterMask;

	if (m_useGhostObjectSweepTest)
	{
		m_ghostObject->convexSweepTest (m_convexShape, start, end, callback, world->getDispatchInfo().m_allowedCcdPenetration);
	}
	else
	{
		world->convexSweepTest (m_convexShape, start, end, callback);
	}

	if (callback.hasHit())
	{
		// Only modify the position if the hit was a slope and not a wall or ceiling.
		if(callback.m_hitNormalWorld.dot(getUpAxisDirections()[m_upAxis]) > 0.0)
		{
			// we moved up only a fraction of the step height
			m_currentStepOffset = m_stepHeight * callback.m_closestHitFraction;
			m_currentPosition.setInterpolate3 (m_currentPosition, m_targetPosition, callback.m_closestHitFraction);
		}
		m_verticalVelocity = 0.0;
		m_verticalOffset = 0.0;
	} else {
		m_currentStepOffset = m_stepHeight;
		m_currentPosition = m_targetPosition;
	}
}

void CharacterControllerPhysics::updateTargetPositionBasedOnCollision (const btVector3& hitNormal, btScalar tangentMag, btScalar normalMag)
{
	btVector3 movementDirection = m_targetPosition - m_currentPosition;
	btScalar movementLength = movementDirection.length();
	if (movementLength>SIMD_EPSILON)
	{
		movementDirection.normalize();

		btVector3 reflectDir = computeReflectionDirection (movementDirection, hitNormal);
		reflectDir.normalize();

		btVector3 parallelDir, perpindicularDir;

		parallelDir = parallelComponent (reflectDir, hitNormal);
		perpindicularDir = perpindicularComponent (reflectDir, hitNormal);

		m_targetPosition = m_currentPosition;
		if (0)//tangentMag != 0.0)
		{
			btVector3 parComponent = parallelDir * btScalar (tangentMag*movementLength);
//			printf("parComponent=%f,%f,%f\n",parComponent[0],parComponent[1],parComponent[2]);
			m_targetPosition +=  parComponent;
		}

		if (normalMag != 0.0)
		{
			btVector3 perpComponent = perpindicularDir * btScalar (normalMag*movementLength);
//			printf("perpComponent=%f,%f,%f\n",perpComponent[0],perpComponent[1],perpComponent[2]);
			m_targetPosition += perpComponent;
		}
	} else
	{
//		printf("movementLength don't normalize a zero vector\n");
	}
}

void CharacterControllerPhysics::stepForwardAndStrafe ( btCollisionWorld* collisionWorld, const btVector3& walkMove, btScalar dt)
{
	// printf("m_normalizedDirection=%f,%f,%f\n",
	// 	m_normalizedDirection[0],m_normalizedDirection[1],m_normalizedDirection[2]);
	// phase 2: forward and strafe
	btTransform start, end;
	m_targetPosition = m_currentPosition + walkMove;

	start.setIdentity ();
	end.setIdentity ();

	btScalar fraction = 1.0;
	btScalar distance2 = (m_currentPosition-m_targetPosition).length2();
//	printf("distance2=%f\n",distance2);

	if (m_touchingContact)
	{
		if (m_normalizedDirection.dot(m_touchingNormal) > btScalar(0.0))
		{
		    //interferes with step movement
			//updateTargetPositionBasedOnCollision (m_touchingNormal);
		}
	}

	int maxIter = 10;

	while (fraction > btScalar(0.01) && maxIter-- > 0)
	{
		start.setOrigin (m_currentPosition);
		end.setOrigin (m_targetPosition);
		btVector3 sweepDirNegative(m_currentPosition - m_targetPosition);

		btKinematicClosestNotMeConvexResultCallback callback (m_ghostObject, sweepDirNegative, btScalar(0.0));
		callback.m_collisionFilterGroup = getGhostObject()->getBroadphaseHandle()->m_collisionFilterGroup;
		callback.m_collisionFilterMask = getGhostObject()->getBroadphaseHandle()->m_collisionFilterMask;


		btScalar margin = m_convexShape->getMargin();
		m_convexShape->setMargin(margin + m_addedMargin);


		if (m_useGhostObjectSweepTest)
		{
			m_ghostObject->convexSweepTest (m_convexShape, start, end, callback, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);
		} else
		{
			collisionWorld->convexSweepTest (m_convexShape, start, end, callback, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);
		}
        if (callback.hasHit() == false && !m_wasJumping) {
            callback.m_collisionFilterGroup = m_BorderDetectionShapeGroup;
            callback.m_collisionFilterMask = m_BorderDetectionShapeMask;
            start.setOrigin (m_currentPosition + m_vBorderDetectionShapeOffset);
            end.setOrigin (m_targetPosition + m_vBorderDetectionShapeOffset);
            /*if (m_useGhostObjectSweepTest)
            {
                m_ghostObject->convexSweepTest (m_convexShapeForBorderDetection, start, end, callback, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);
            } else*/
            {
                collisionWorld->convexSweepTest (m_convexShapeForBorderDetection, start, end, callback, collisionWorld->getDispatchInfo().m_allowedCcdPenetration + m_fCurrentBorderCcdPenetration);
            }
            if (callback.hasHit()) {
                // hit with wall
                m_fCurrentBorderCcdPenetration += BORDER_CCD_PENETRATION_SPEED_ADD_FACTOR * BORDER_CCD_PENETRATION_SPEED * dt;
            }
        }
		m_convexShape->setMargin(margin);


		fraction -= callback.m_closestHitFraction;

		if (callback.hasHit())
		{
			// we moved only a fraction
			btScalar hitDistance;
			hitDistance = (callback.m_hitPointWorld - m_currentPosition).length();

//			m_currentPosition.setInterpolate3 (m_currentPosition, m_targetPosition, callback.m_closestHitFraction);

			updateTargetPositionBasedOnCollision (callback.m_hitNormalWorld);
			btVector3 currentDir = m_targetPosition - m_currentPosition;
			distance2 = currentDir.length2();
			if (distance2 > SIMD_EPSILON)
			{
				currentDir.normalize();
				/* See Quake2: "If velocity is against original velocity, stop ead to avoid tiny oscilations in sloping corners." */
				if (currentDir.dot(m_normalizedDirection) <= btScalar(0.0))
				{
					break;
				}
			} else
			{
//				printf("currentDir: don't normalize a zero vector\n");
				break;
			}

		} else {
			// we moved whole way
			m_currentPosition = m_targetPosition;
		}

	//	if (callback.m_closestHitFraction == 0.f)
	//		break;

	}
}

void CharacterControllerPhysics::stepDown ( btCollisionWorld* collisionWorld, btScalar dt)
{
	btTransform start, end, end_double;
	bool runonce = false;

	// phase 3: down
	/*btScalar additionalDownStep = (m_wasOnGround && !onGround()) ? m_stepHeight : 0.0;
	btVector3 step_drop = getUpAxisDirections()[m_upAxis] * (m_currentStepOffset + additionalDownStep);
	btScalar downVelocity = (additionalDownStep == 0.0 && m_verticalVelocity<0.0?-m_verticalVelocity:0.0) * dt;
	btVector3 gravity_drop = getUpAxisDirections()[m_upAxis] * downVelocity;
	m_targetPosition -= (step_drop + gravity_drop);*/

	btVector3 orig_position = m_targetPosition;

	btScalar downVelocity = (m_verticalVelocity<0.f?-m_verticalVelocity:0.f) * dt;

	if(downVelocity > 0.0 && downVelocity > m_fallSpeed
		&& (m_wasOnGround || !m_wasJumping))
		downVelocity = m_fallSpeed;

	btVector3 step_drop = getUpAxisDirections()[m_upAxis] * (m_currentStepOffset + downVelocity);
	m_targetPosition -= step_drop;

	btKinematicClosestNotMeConvexResultCallback callback (m_ghostObject, getUpAxisDirections()[m_upAxis], m_maxSlopeCosine);
        callback.m_collisionFilterGroup = getGhostObject()->getBroadphaseHandle()->m_collisionFilterGroup;
        callback.m_collisionFilterMask = getGhostObject()->getBroadphaseHandle()->m_collisionFilterMask;

        btKinematicClosestNotMeConvexResultCallback callback2 (m_ghostObject, getUpAxisDirections()[m_upAxis], m_maxSlopeCosine);
        callback2.m_collisionFilterGroup = getGhostObject()->getBroadphaseHandle()->m_collisionFilterGroup;
        callback2.m_collisionFilterMask = getGhostObject()->getBroadphaseHandle()->m_collisionFilterMask;

	while (1)
	{
		start.setIdentity ();
		end.setIdentity ();

		end_double.setIdentity ();

		start.setOrigin (m_currentPosition);
		end.setOrigin (m_targetPosition);

		//set double test for 2x the step drop, to check for a large drop vs small drop
		end_double.setOrigin (m_targetPosition - step_drop);

		if (m_useGhostObjectSweepTest)
		{
			m_ghostObject->convexSweepTest (m_convexShape, start, end, callback, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);

			if (!callback.hasHit())
			{
				//test a double fall height, to see if the character should interpolate it's fall (full) or not (partial)
				m_ghostObject->convexSweepTest (m_convexShape, start, end_double, callback2, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);
			}
		} else
		{
			collisionWorld->convexSweepTest (m_convexShape, start, end, callback, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);

			if (!callback.hasHit())
					{
							//test a double fall height, to see if the character should interpolate it's fall (large) or not (small)
							collisionWorld->convexSweepTest (m_convexShape, start, end_double, callback2, collisionWorld->getDispatchInfo().m_allowedCcdPenetration);
					}
		}

		btScalar downVelocity2 = (m_verticalVelocity<0.f?-m_verticalVelocity:0.f) * dt;
		bool has_hit = false;
		if (bounce_fix == true)
			has_hit = callback.hasHit() || callback2.hasHit();
		else
			has_hit = callback2.hasHit();

		if(downVelocity2 > 0.0 && downVelocity2 < m_stepHeight && has_hit == true && runonce == false
					&& (m_wasOnGround || !m_wasJumping))
		{
			//redo the velocity calculation when falling a small amount, for fast stairs motion
			//for larger falls, use the smoother/slower interpolated movement by not touching the target position

			m_targetPosition = orig_position;
					downVelocity = m_stepHeight;

				btVector3 step_drop = getUpAxisDirections()[m_upAxis] * (m_currentStepOffset + downVelocity);
			m_targetPosition -= step_drop;
			runonce = true;
			continue; //re-run previous tests
		}
		break;
	}

	if (callback.hasHit() || runonce == true)
	{
		// we dropped a fraction of the height -> hit floor

		btScalar fraction = (m_currentPosition.getY() - callback.m_hitPointWorld.getY()) / 2;

		//printf("hitpoint: %g - pos %g\n", callback.m_hitPointWorld.getY(), m_currentPosition.getY());

		if (bounce_fix == true)
		{
			if (full_drop == true)
                                m_currentPosition.setInterpolate3 (m_currentPosition, m_targetPosition, callback.m_closestHitFraction);
                        else
                                //due to errors in the closestHitFraction variable when used with large polygons, calculate the hit fraction manually
                                m_currentPosition.setInterpolate3 (m_currentPosition, m_targetPosition, fraction);
		}
		else
			m_currentPosition.setInterpolate3 (m_currentPosition, m_targetPosition, callback.m_closestHitFraction);

		full_drop = false;

		m_verticalVelocity = 0.0;
		m_verticalOffset = 0.0;
		m_wasJumping = false;
	} else {
		// we dropped the full height

		full_drop = true;

		if (bounce_fix == true)
		{
			downVelocity = (m_verticalVelocity<0.f?-m_verticalVelocity:0.f) * dt;
			if (downVelocity > m_fallSpeed && (m_wasOnGround || !m_wasJumping))
			{
				m_targetPosition += step_drop; //undo previous target change
				downVelocity = m_fallSpeed;
				step_drop = getUpAxisDirections()[m_upAxis] * (m_currentStepOffset + downVelocity);
				m_targetPosition -= step_drop;
			}
		}
		//printf("full drop - %g, %g\n", m_currentPosition.getY(), m_targetPosition.getY());

		m_currentPosition = m_targetPosition;
	}
}


void CharacterControllerPhysics::setWalkDirection
(
const btVector3& walkDirection
)
{
	m_useWalkDirection = true;
	m_walkDirection = walkDirection;
	if (m_walkDirection.isZero()) {
        m_normalizedDirection = m_walkDirection;
	}
	else {
        m_normalizedDirection = getNormalizedVector(m_walkDirection);
	}
}



void CharacterControllerPhysics::setVelocityForTimeInterval
(
const btVector3& velocity,
btScalar timeInterval
)
{
//	printf("setVelocity!\n");
//	printf("  interval: %f\n", timeInterval);
//	printf("  velocity: (%f, %f, %f)\n",
//		 velocity.x(), velocity.y(), velocity.z());

	m_useWalkDirection = false;
	m_walkDirection = velocity;
	m_normalizedDirection = getNormalizedVector(m_walkDirection);
	m_velocityTimeInterval = timeInterval;
}



void CharacterControllerPhysics::reset ( btCollisionWorld* collisionWorld )
{
        m_verticalVelocity = 0.0;
        m_verticalOffset = 0.0;
        m_wasOnGround = false;
        m_wasJumping = false;
        m_walkDirection.setValue(0,0,0);
        m_velocityTimeInterval = 0.0;

        //clear pair cache
        btHashedOverlappingPairCache *cache = m_ghostObject->getOverlappingPairCache();
        while (cache->getOverlappingPairArray().size() > 0)
        {
                cache->removeOverlappingPair(cache->getOverlappingPairArray()[0].m_pProxy0, cache->getOverlappingPairArray()[0].m_pProxy1, collisionWorld->getDispatcher());
        }
}

void CharacterControllerPhysics::warp (const btVector3& origin)
{
	btTransform xform;
	xform.setIdentity();
	xform.setOrigin (origin);
	m_ghostObject->setWorldTransform (xform);
}


void CharacterControllerPhysics::preStep (  btCollisionWorld* collisionWorld)
{

	int numPenetrationLoops = 0;
	m_touchingContact = false;
	while (recoverFromPenetration (collisionWorld))
	{
		numPenetrationLoops++;
		m_touchingContact = true;
		if (numPenetrationLoops > 4)
		{
			//printf("character could not recover from penetration = %d\n", numPenetrationLoops);
			break;
		}
	}

	m_currentPosition = m_ghostObject->getWorldTransform().getOrigin();
	m_targetPosition = m_currentPosition;
//	printf("m_targetPosition=%f,%f,%f\n",m_targetPosition[0],m_targetPosition[1],m_targetPosition[2]);


}

#include <stdio.h>

void CharacterControllerPhysics::playerStep (  btCollisionWorld* collisionWorld, btScalar dt)
{
//	printf("playerStep(): ");
//	printf("  dt = %f", dt);
    m_fCurrentBorderCcdPenetration -= BORDER_CCD_PENETRATION_SPEED * dt;
    if (m_fCurrentBorderCcdPenetration < 0) {m_fCurrentBorderCcdPenetration = 0;}
	// quick check...
	if (!m_useWalkDirection && m_velocityTimeInterval <= 0.0) {
//		printf("\n");
		return;		// no motion
	}

	m_wasOnGround = onGround();

	// Update fall velocity.
	m_verticalVelocity -= m_gravity * dt;
	if(m_verticalVelocity > 0.0 && m_verticalVelocity > m_jumpSpeed)
	{
		m_verticalVelocity = m_jumpSpeed;
	}
	if(m_verticalVelocity < 0.0 && btFabs(m_verticalVelocity) > btFabs(m_fallSpeed))
	{
		m_verticalVelocity = -btFabs(m_fallSpeed);
	}
	m_verticalOffset = m_verticalVelocity * dt;


	btTransform xform;
	xform = m_ghostObject->getWorldTransform ();

//	printf("walkDirection(%f,%f,%f)\n",walkDirection[0],walkDirection[1],walkDirection[2]);
//	printf("walkSpeed=%f\n",walkSpeed);

	stepUp (collisionWorld);
	if (m_useWalkDirection) {
		stepForwardAndStrafe (collisionWorld, m_walkDirection, dt);
	} else {
		//printf("  time: %f", m_velocityTimeInterval);
		// still have some time left for moving!
		btScalar dtMoving =
			(dt < m_velocityTimeInterval) ? dt : m_velocityTimeInterval;
		m_velocityTimeInterval -= dt;

		// how far will we move while we are moving?
		btVector3 move = m_walkDirection * dtMoving;

		//printf("  dtMoving: %f", dtMoving);

		// okay, step
		stepForwardAndStrafe(collisionWorld, move, dt);
	}
	stepDown (collisionWorld, dt);

	// printf("\n");

	xform.setOrigin (m_currentPosition);
	m_ghostObject->setWorldTransform (xform);
}

void CharacterControllerPhysics::setFallSpeed (btScalar fallSpeed)
{
	m_fallSpeed = fallSpeed;
}

void CharacterControllerPhysics::setJumpSpeed (btScalar jumpSpeed)
{
	m_jumpSpeed = jumpSpeed;
}

void CharacterControllerPhysics::setMaxJumpHeight (btScalar maxJumpHeight)
{
	m_maxJumpHeight = maxJumpHeight;
}

bool CharacterControllerPhysics::canJump () const
{
	return onGround();
}

void CharacterControllerPhysics::jump ()
{
	if (!canJump())
		return;

	m_verticalVelocity = m_jumpSpeed;
	m_wasJumping = true;

#if 0
	currently no jumping.
	btTransform xform;
	m_rigidBody->getMotionState()->getWorldTransform (xform);
	btVector3 up = xform.getBasis()[1];
	up.normalize ();
	btScalar magnitude = (btScalar(1.0)/m_rigidBody->getInvMass()) * btScalar(8.0);
	m_rigidBody->applyCentralImpulse (up * magnitude);
#endif
}

void CharacterControllerPhysics::setGravity(btScalar gravity)
{
	m_gravity = gravity;
}

btScalar CharacterControllerPhysics::getGravity() const
{
	return m_gravity;
}

void CharacterControllerPhysics::setMaxSlope(btScalar slopeRadians)
{
	m_maxSlopeRadians = slopeRadians;
	m_maxSlopeCosine = btCos(slopeRadians);
}

btScalar CharacterControllerPhysics::getMaxSlope() const
{
	return m_maxSlopeRadians;
}

bool CharacterControllerPhysics::onGround () const
{
	return m_verticalVelocity == 0.0 && m_verticalOffset == 0.0;
}


btVector3* CharacterControllerPhysics::getUpAxisDirections()
{
	static btVector3 sUpAxisDirection[3] = { btVector3(1.0f, 0.0f, 0.0f), btVector3(0.0f, 1.0f, 0.0f), btVector3(0.0f, 0.0f, 1.0f) };

	return sUpAxisDirection;
}

void CharacterControllerPhysics::debugDraw(btIDebugDraw* debugDrawer)
{
    btCylinderShape *pCylShape = dynamic_cast<btCylinderShape*>(m_convexShapeForBorderDetection);
    if (pCylShape) {
        btScalar radius = pCylShape->getHalfExtentsWithoutMargin()[0];
        btScalar halfheight = pCylShape->getHalfExtentsWithoutMargin()[1];
        btTransform t(m_ghostObject->getWorldTransform());
        t.setOrigin(t.getOrigin() + m_vBorderDetectionShapeOffset);
        debugDrawer->drawCylinder(radius, halfheight, pCylShape->getUpAxis(), t, btVector3(1, 0, 0));
    }
}

void CharacterControllerPhysics::setUpInterpolate(bool value)
{
	m_interpolateUp = value;
}
