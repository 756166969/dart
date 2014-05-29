/*
 * Copyright (c) 2011-2014, Georgia Tech Research Corporation
 * All rights reserved.
 *
 * Author(s): Sehoon Ha <sehoon.ha@gmail.com>,
 *            Jeongseok Lee <jslee02@gmail.com>
 *
 * Georgia Tech Graphics Lab and Humanoid Robotics Lab
 *
 * Directed by Prof. C. Karen Liu and Prof. Mike Stilman
 * <karenliu@cc.gatech.edu> <mstilman@cc.gatech.edu>
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

#include "dart/dynamics/Skeleton.h"

#include <algorithm>
#include <queue>
#include <string>
#include <vector>

#include "dart/common/Console.h"
#include "dart/math/Geometry.h"
#include "dart/math/Helpers.h"
#include "dart/dynamics/BodyNode.h"
#include "dart/dynamics/SoftBodyNode.h"
#include "dart/dynamics/PointMass.h"
#include "dart/dynamics/GenCoord.h"
#include "dart/dynamics/Joint.h"
#include "dart/dynamics/Marker.h"

namespace dart {
namespace dynamics {

//==============================================================================
Skeleton::Skeleton(const std::string& _name)
  : GenCoordSystem(),
    mName(_name),
    mEnabledSelfCollisionCheck(false),
    mEnabledAdjacentBodyCheck(false),
    mTimeStep(0.001),
    mGravity(Eigen::Vector3d(0.0, 0.0, -9.81)),
    mTotalMass(0.0),
    mIsMobile(true),
    mIsArticulatedInertiaDirty(true),
    mIsMassMatrixDirty(true),
    mIsAugMassMatrixDirty(true),
    mIsInvMassMatrixDirty(true),
    mIsInvAugMassMatrixDirty(true),
    mIsCoriolisVectorDirty(true),
    mIsGravityForceVectorDirty(true),
    mIsCombinedVectorDirty(true),
    mIsExternalForceVectorDirty(true),
    mIsDampingForceVectorDirty(true),
    mIsImpulseApplied(false),
    mUnionRootSkeleton(this),
    mUnionSize(1)
{
}

Skeleton::~Skeleton() {
  for (std::vector<BodyNode*>::const_iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
    delete (*it);
}

void Skeleton::setName(const std::string& _name) {
  mName = _name;
}

const std::string& Skeleton::getName() const {
  return mName;
}

void Skeleton::enableSelfCollision(bool _enableAdjecentBodyCheck)
{
  mEnabledSelfCollisionCheck = true;
  mEnabledAdjacentBodyCheck = _enableAdjecentBodyCheck;
}

void Skeleton::disableSelfCollision()
{
  mEnabledSelfCollisionCheck = false;
  mEnabledAdjacentBodyCheck = false;
}

bool Skeleton::isEnabledSelfCollisionCheck() const
{
  return mEnabledSelfCollisionCheck;
}

bool Skeleton::isEnabledAdjacentBodyCheck() const
{
  return mEnabledAdjacentBodyCheck;
}

void Skeleton::setMobile(bool _isMobile) {
  mIsMobile = _isMobile;
}

bool Skeleton::isMobile() const {
  return mIsMobile;
}

void Skeleton::setTimeStep(double _timeStep) {
  assert(_timeStep > 0.0);
  mTimeStep = _timeStep;
}

double Skeleton::getTimeStep() const {
  return mTimeStep;
}

void Skeleton::setGravity(const Eigen::Vector3d& _gravity) {
  mGravity = _gravity;
}

const Eigen::Vector3d& Skeleton::getGravity() const {
  return mGravity;
}

double Skeleton::getMass() const {
  return mTotalMass;
}

//==============================================================================
void Skeleton::init(double _timeStep, const Eigen::Vector3d& _gravity)
{
  // Set timestep and gravity
  setTimeStep(_timeStep);
  setGravity(_gravity);

  // Rearrange the list of body nodes with BFS (Breadth First Search)
  std::queue<BodyNode*> queue;
  queue.push(mBodyNodes[0]);
  mBodyNodes.clear();
  while (!queue.empty())
  {
    BodyNode* itBodyNode = queue.front();
    queue.pop();
    mBodyNodes.push_back(itBodyNode);
    for (int i = 0; i < itBodyNode->getNumChildBodyNodes(); ++i)
      queue.push(itBodyNode->getChildBodyNode(i));
  }

  // Initialize body nodes and generalized coordinates
  mGenCoords.clear();
  for (int i = 0; i < getNumBodyNodes(); ++i)
  {
//    mBodyNodes[i]->aggregateGenCoords(&mGenCoords);
    mBodyNodes[i]->init(this, i);
  }

  // Compute transformations, velocities, and partial accelerations
  computeForwardDynamicsRecursionPartA();

  // Set dimension of dynamics quantities
  int dof = getDof();
  mM    = Eigen::MatrixXd::Zero(dof, dof);
  mAugM = Eigen::MatrixXd::Zero(dof, dof);
  mInvM = Eigen::MatrixXd::Zero(dof, dof);
  mInvAugM = Eigen::MatrixXd::Zero(dof, dof);
  mCvec = Eigen::VectorXd::Zero(dof);
  mG    = Eigen::VectorXd::Zero(dof);
  mCg   = Eigen::VectorXd::Zero(dof);
  mFext = Eigen::VectorXd::Zero(dof);
  mFc   = Eigen::VectorXd::Zero(dof);
  mFd   = Eigen::VectorXd::Zero(dof);

  // Clear external/internal force
  clearExternalForces();
  clearInternalForces();

  // Calculate mass
  mTotalMass = 0.0;
  for (int i = 0; i < getNumBodyNodes(); i++)
    mTotalMass += getBodyNode(i)->getMass();
}

//==============================================================================
void Skeleton::addBodyNode(BodyNode* _body)
{
  assert(_body && _body->getParentJoint());

  mBodyNodes.push_back(_body);

  SoftBodyNode* softBodyNode = dynamic_cast<SoftBodyNode*>(_body);
  if (softBodyNode)
    mSoftBodyNodes.push_back(softBodyNode);
}

int Skeleton::getNumBodyNodes() const {
  return static_cast<int>(mBodyNodes.size());
}

int Skeleton::getNumRigidBodyNodes() const {
  return mBodyNodes.size() - mSoftBodyNodes.size();
}

int Skeleton::getNumSoftBodyNodes() const {
  return mSoftBodyNodes.size();
}

BodyNode* Skeleton::getRootBodyNode() const {
  // We assume that the first element of body nodes is root.
  return mBodyNodes[0];
}

BodyNode* Skeleton::getBodyNode(int _idx) const {
  return mBodyNodes[_idx];
}

SoftBodyNode* Skeleton::getSoftBodyNode(int _idx) const {
  assert(0 <= _idx && _idx < mSoftBodyNodes.size());
  return mSoftBodyNodes[_idx];
}

BodyNode* Skeleton::getBodyNode(const std::string& _name) const {
  assert(!_name.empty());

  for (std::vector<BodyNode*>::const_iterator itrBody = mBodyNodes.begin();
       itrBody != mBodyNodes.end(); ++itrBody) {
    if ((*itrBody)->getName() == _name)
      return *itrBody;
  }

  return NULL;
}

SoftBodyNode* Skeleton::getSoftBodyNode(const std::string& _name) const {
  assert(!_name.empty());
  for (std::vector<SoftBodyNode*>::const_iterator itrSoftBodyNode =
       mSoftBodyNodes.begin();
       itrSoftBodyNode != mSoftBodyNodes.end(); ++itrSoftBodyNode) {
    if ((*itrSoftBodyNode)->getName() == _name)
      return *itrSoftBodyNode;
  }

  return NULL;
}

Joint* Skeleton::getJoint(int _idx) const {
  return mBodyNodes[_idx]->getParentJoint();
}

Joint* Skeleton::getJoint(const std::string& _name) const {
  assert(!_name.empty());

  for (std::vector<BodyNode*>::const_iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it) {
    if ((*it)->getParentJoint()->getName() == _name)
      return (*it)->getParentJoint();
  }

  return NULL;
}

Marker* Skeleton::getMarker(const std::string& _name) const {
  assert(!_name.empty());

  for (std::vector<BodyNode*>::const_iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it) {
    for (int i = 0; i < (*it)->getNumMarkers(); ++i) {
      if ((*it)->getMarker(i)->getName() == _name)
        return (*it)->getMarker(i);
    }
  }

  return NULL;
}

//==============================================================================
Eigen::VectorXd Skeleton::getConfigSegs(const std::vector<int>& _id) const
{
  Eigen::VectorXd q(_id.size());

  for (unsigned int i = 0; i < _id.size(); i++)
    q[i] = mGenCoords[_id[i]]->getPos();

  return q;
}

//==============================================================================
void Skeleton::setConfigSegs(const std::vector<int>& _id,
                             const Eigen::VectorXd& _configs,
                             bool _updateTransforms,
                             bool _updateVels,
                             bool _updateAccs)
{
  for ( unsigned int i = 0; i < _id.size(); i++ )
    mGenCoords[_id[i]]->setPos(_configs(i));

  computeForwardKinematics(_updateTransforms, _updateVels, _updateAccs);
}

//==============================================================================
void Skeleton::setPositions(const Eigen::VectorXd& _configs,
                          bool _updateTransforms,
                          bool _updateVels,
                          bool _updateAccs)
{
  GenCoordSystem::setPositions(_configs);

  computeForwardKinematics(_updateTransforms, _updateVels, _updateAccs);
}

//==============================================================================
void Skeleton::setVelocities(const Eigen::VectorXd& _genVels,
                          bool _updateVels,
                          bool _updateAccs)
{
  GenCoordSystem::setVelocities(_genVels);

  computeForwardKinematics(false, _updateVels, _updateAccs);
}

//==============================================================================
void Skeleton::setAccelerations(const Eigen::VectorXd& _genAccs, bool _updateAccs)
{
  GenCoordSystem::setAccelerations(_genAccs);

  computeForwardKinematics(false, false, _updateAccs);
}

//==============================================================================
void Skeleton::setState(const Eigen::VectorXd& _state,
                        bool _updateTransforms,
                        bool _updateVels,
                        bool _updateAccs)
{
  assert(_state.size() % 2 == 0);
  GenCoordSystem::setPositions(_state.head(_state.size() / 2));
  GenCoordSystem::setVelocities(_state.tail(_state.size() / 2));

  computeForwardKinematics(_updateTransforms, _updateVels, _updateAccs);
}

//==============================================================================
Eigen::VectorXd Skeleton::getState() const
{
  Eigen::VectorXd state(2 * mGenCoords.size());
  state << getConfigs(), getGenVels();
  return state;
}

//==============================================================================
void Skeleton::integrateConfigs(double _dt)
{
  for (size_t i = 0; i < mBodyNodes.size(); ++i)
    mBodyNodes[i]->getParentJoint()->integratePositions(_dt);

  for (size_t i = 0; i < mSoftBodyNodes.size(); ++i)
  {
    for (size_t j = 0; j < mSoftBodyNodes[i]->getNumPointMasses(); ++j)
      mSoftBodyNodes[i]->getPointMass(j)->integrateConfigs(_dt);
  }
}

//==============================================================================
void Skeleton::integrateGenVels(double _dt)
{
  for (size_t i = 0; i < mBodyNodes.size(); ++i)
    mBodyNodes[i]->getParentJoint()->integrateVelocities(_dt);

  for (size_t i = 0; i < mSoftBodyNodes.size(); ++i)
  {
    for (size_t j = 0; j < mSoftBodyNodes[i]->getNumPointMasses(); ++j)
      mSoftBodyNodes[i]->getPointMass(j)->integrateGenVels(_dt);
  }
}

//==============================================================================
void Skeleton::computeForwardKinematics(bool _updateTransforms,
                                        bool _updateVels,
                                        bool _updateAccs)
{
  if (_updateTransforms)
  {
    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
         it != mBodyNodes.end(); ++it)
    {
      (*it)->updateTransform();
    }
  }

  if (_updateVels)
  {
    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
         it != mBodyNodes.end(); ++it)
    {
      (*it)->updateVelocity();
      (*it)->updatePartialAcceleration();
    }
  }

  if (_updateAccs)
  {
    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
         it != mBodyNodes.end(); ++it)
    {
      (*it)->updateAcceleration();
    }
  }

  mIsArticulatedInertiaDirty = true;
  mIsMassMatrixDirty = true;
  mIsAugMassMatrixDirty = true;
  mIsInvMassMatrixDirty = true;
  mIsInvAugMassMatrixDirty = true;
  mIsCoriolisVectorDirty = true;
  mIsGravityForceVectorDirty = true;
  mIsCombinedVectorDirty = true;
  mIsExternalForceVectorDirty = true;
//  mIsDampingForceVectorDirty = true;

  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->mIsBodyJacobianDirty = true;
    (*it)->mIsBodyJacobianTimeDerivDirty = true;
  }
}

const Eigen::MatrixXd& Skeleton::getMassMatrix() {
  if (mIsMassMatrixDirty)
    updateMassMatrix();
  return mM;
}

const Eigen::MatrixXd& Skeleton::getAugMassMatrix() {
  if (mIsAugMassMatrixDirty)
    updateAugMassMatrix();
  return mAugM;
}

const Eigen::MatrixXd& Skeleton::getInvMassMatrix() {
  if (mIsInvMassMatrixDirty)
    updateInvMassMatrix();
  return mInvM;
}

const Eigen::MatrixXd& Skeleton::getInvAugMassMatrix() {
  if (mIsInvAugMassMatrixDirty)
    updateInvAugMassMatrix();
  return mInvAugM;
}

const Eigen::VectorXd& Skeleton::getCoriolisForceVector() {
  if (mIsCoriolisVectorDirty)
    updateCoriolisForceVector();
  return mCvec;
}

const Eigen::VectorXd& Skeleton::getGravityForceVector() {
  if (mIsGravityForceVectorDirty)
    updateGravityForceVector();
  return mG;
}

const Eigen::VectorXd& Skeleton::getCombinedVector() {
  if (mIsCombinedVectorDirty)
    updateCombinedVector();
  return mCg;
}

const Eigen::VectorXd& Skeleton::getExternalForceVector() {
  if (mIsExternalForceVectorDirty)
    updateExternalForceVector();
  return mFext;
}

Eigen::VectorXd Skeleton::getInternalForceVector() const {
  return getGenForces();
}

//const Eigen::VectorXd& Skeleton::getDampingForceVector() {
//  if (mIsDampingForceVectorDirty)
//    updateDampingForceVector();
//  return mFd;
//}

const Eigen::VectorXd& Skeleton::getConstraintForceVector() {
  return mFc;
}

void Skeleton::draw(renderer::RenderInterface* _ri,
                    const Eigen::Vector4d& _color,
                    bool _useDefaultColor) const {
  getRootBodyNode()->draw(_ri, _color, _useDefaultColor);
}

void Skeleton::drawMarkers(renderer::RenderInterface* _ri,
                           const Eigen::Vector4d& _color,
                           bool _useDefaultColor) const {
  getRootBodyNode()->drawMarkers(_ri, _color, _useDefaultColor);
}

void Skeleton::updateMassMatrix() {
  assert(mM.cols() == getDof() && mM.rows() == getDof());
  assert(getDof() > 0);

  mM.setZero();

  // Backup the origianl internal force
  Eigen::VectorXd originalGenAcceleration = getGenAccs();

  int dof = getDof();
  Eigen::VectorXd e = Eigen::VectorXd::Zero(dof);
  for (int j = 0; j < dof; ++j) {
    e[j] = 1.0;
    GenCoordSystem::setAccelerations(e);

    // Prepare cache data
    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
         it != mBodyNodes.end(); ++it) {
      (*it)->updateMassMatrix();
    }

    // Mass matrix
    //    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
    //         it != mBodyNodes.end(); ++it)
    for (int i = mBodyNodes.size() - 1; i > -1 ; --i) {
      mBodyNodes[i]->aggregateMassMatrix(&mM, j);
      int localDof = mBodyNodes[i]->mParentJoint->getDof();
      if (localDof > 0) {
        int iStart =
            mBodyNodes[i]->mParentJoint->getIndexInSkeleton(0);
        if (iStart + localDof < j)
          break;
      }
    }

    e[j] = 0.0;
  }
  mM.triangularView<Eigen::StrictlyUpper>() = mM.transpose();

  // Restore the origianl generalized accelerations
  setAccelerations(originalGenAcceleration, false);

  mIsMassMatrixDirty = false;
}

void Skeleton::updateAugMassMatrix() {
  assert(mAugM.cols() == getDof() && mAugM.rows() == getDof());
  assert(getDof() > 0);

  mAugM.setZero();

  // Backup the origianl internal force
  Eigen::VectorXd originalGenAcceleration = getGenAccs();

  int dof = getDof();
  Eigen::VectorXd e = Eigen::VectorXd::Zero(dof);
  for (int j = 0; j < dof; ++j) {
    e[j] = 1.0;
    GenCoordSystem::setAccelerations(e);

    // Prepare cache data
    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
         it != mBodyNodes.end(); ++it) {
      (*it)->updateMassMatrix();
    }

    // Mass matrix
    //    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
    //         it != mBodyNodes.end(); ++it)
    for (int i = mBodyNodes.size() - 1; i > -1 ; --i) {
      mBodyNodes[i]->aggregateAugMassMatrix(&mAugM, j, mTimeStep);
      int localDof = mBodyNodes[i]->mParentJoint->getDof();
      if (localDof > 0) {
        int iStart =
            mBodyNodes[i]->mParentJoint->getIndexInSkeleton(0);
        if (iStart + localDof < j)
          break;
      }
    }

    e[j] = 0.0;
  }
  mAugM.triangularView<Eigen::StrictlyUpper>() = mAugM.transpose();

  // Restore the origianl internal force
  GenCoordSystem::setAccelerations(originalGenAcceleration);

  mIsAugMassMatrixDirty = false;
}

void Skeleton::updateInvMassMatrix() {
  assert(mInvM.cols() == getDof() &&
         mInvM.rows() == getDof());
  assert(getDof() > 0);

  // We don't need to set mInvM as zero matrix as long as the below is correct
  // mInvM.setZero();

  // Backup the origianl internal force
  Eigen::VectorXd originalInternalForce = getGenForces();

  if (mIsArticulatedInertiaDirty)
  {
    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
         it != mBodyNodes.rend(); ++it)
    {
      (*it)->updateArtInertia(mTimeStep);
    }

    mIsArticulatedInertiaDirty = false;
  }

  int dof = getDof();
  Eigen::VectorXd e = Eigen::VectorXd::Zero(dof);
  for (int j = 0; j < dof; ++j) {
    e[j] = 1.0;
    setGenForces(e);

    // Prepare cache data
    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
         it != mBodyNodes.rend(); ++it) {
      (*it)->updateInvMassMatrix();
    }

    // Inverse of mass matrix
    //    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
    //         it != mBodyNodes.end(); ++it)
    for (int i = 0; i < mBodyNodes.size(); ++i) {
      mBodyNodes[i]->aggregateInvMassMatrix(&mInvM, j);
      int localDof = mBodyNodes[i]->mParentJoint->getDof();
      if (localDof > 0) {
        int iStart =
            mBodyNodes[i]->mParentJoint->getIndexInSkeleton(0);
        if (iStart + localDof > j)
          break;
      }
    }

    e[j] = 0.0;
  }
  mInvM.triangularView<Eigen::StrictlyLower>() = mInvM.transpose();

  // Restore the origianl internal force
  setGenForces(originalInternalForce);

  mIsInvMassMatrixDirty = false;
}

void Skeleton::updateInvAugMassMatrix() {
  assert(mInvAugM.cols() == getDof() &&
         mInvAugM.rows() == getDof());
  assert(getDof() > 0);

  // We don't need to set mInvM as zero matrix as long as the below is correct
  // mInvM.setZero();

  // Backup the origianl internal force
  Eigen::VectorXd originalInternalForce = getGenForces();

  int dof = getDof();
  Eigen::VectorXd e = Eigen::VectorXd::Zero(dof);
  for (int j = 0; j < dof; ++j) {
    e[j] = 1.0;
    setGenForces(e);

    // Prepare cache data
    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
         it != mBodyNodes.rend(); ++it) {
      (*it)->updateInvAugMassMatrix();
    }

    // Inverse of mass matrix
    //    for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
    //         it != mBodyNodes.end(); ++it)
    for (int i = 0; i < mBodyNodes.size(); ++i) {
      mBodyNodes[i]->aggregateInvAugMassMatrix(&mInvAugM, j, mTimeStep);
      int localDof = mBodyNodes[i]->mParentJoint->getDof();
      if (localDof > 0) {
        int iStart =
            mBodyNodes[i]->mParentJoint->getIndexInSkeleton(0);
        if (iStart + localDof > j)
          break;
      }
    }

    e[j] = 0.0;
  }
  mInvAugM.triangularView<Eigen::StrictlyLower>() = mInvAugM.transpose();

  // Restore the origianl internal force
  setGenForces(originalInternalForce);

  mIsInvAugMassMatrixDirty = false;
}

//==============================================================================
void Skeleton::updateCoriolisForceVector()
{
  assert(mCvec.size() == getDof());
  assert(getDof() > 0);

  mCvec.setZero();

  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->updateCombinedVector();
  }

  for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
       it != mBodyNodes.rend(); ++it)
  {
    (*it)->aggregateCoriolisForceVector(&mCvec);
  }

  mIsCoriolisVectorDirty = false;
}

//==============================================================================
void Skeleton::updateGravityForceVector()
{
  assert(mG.size() == getDof());
  assert(getDof() > 0);

  // Calcualtion mass matrix, M
  mG.setZero();
  for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
       it != mBodyNodes.rend(); ++it)
  {
    (*it)->aggregateGravityForceVector(&mG, mGravity);
  }

  mIsGravityForceVectorDirty = false;
}

void Skeleton::updateCombinedVector() {
  assert(mCg.size() == getDof());
  assert(getDof() > 0);

  mCg.setZero();
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it) {
    (*it)->updateCombinedVector();
  }
  for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
       it != mBodyNodes.rend(); ++it) {
    (*it)->aggregateCombinedVector(&mCg, mGravity);
  }

  mIsCombinedVectorDirty = false;
}

void Skeleton::updateExternalForceVector() {
  assert(mFext.size() == getDof());
  assert(getDof() > 0);

  // Clear external force.
  mFext.setZero();
  for (std::vector<BodyNode*>::reverse_iterator itr = mBodyNodes.rbegin();
       itr != mBodyNodes.rend(); ++itr)
    (*itr)->aggregateExternalForces(&mFext);

  for (std::vector<SoftBodyNode*>::iterator it = mSoftBodyNodes.begin();
       it != mSoftBodyNodes.end(); ++it) {
    double kv = (*it)->getVertexSpringStiffness();
    double ke = (*it)->getEdgeSpringStiffness();

    for (int i = 0; i < (*it)->getNumPointMasses(); ++i)
    {
      PointMass* pm = (*it)->getPointMass(i);
      int nN = pm->getNumConnectedPointMasses();

      // Vertex restoring force
      Eigen::Vector3d Fext = -(kv + nN * ke) * pm->getConfigs()
                             - (mTimeStep * (kv + nN*ke)) * pm->getGenVels();

      // Edge restoring force
      for (int j = 0; j < nN; ++j)
      {
        Fext += ke * (pm->getConnectedPointMass(j)->getConfigs()
                        + mTimeStep * pm->getConnectedPointMass(j)->getGenVels());
      }

      // Assign
      int iStart = pm->getGenCoord(0)->getIndexInSkeleton();
      mFext.segment<3>(iStart) = Fext;
    }
  }

  mIsExternalForceVectorDirty = false;
}

//void Skeleton::updateDampingForceVector() {
//  assert(mFd.size() == getDof());
//  assert(getDof() > 0);

//  // Clear external force.
//  mFd.setZero();

//  for (std::vector<BodyNode*>::iterator itr = mBodyNodes.begin();
//       itr != mBodyNodes.end(); ++itr) {
//    Eigen::VectorXd jointDampingForce =
//        (*itr)->getParentJoint()->getDampingForces();
//    for (int i = 0; i < jointDampingForce.size(); i++) {
//      mFd((*itr)->getParentJoint()->getIndexInSkeleton(0)) =
//          jointDampingForce(i);
//    }
//  }

//  for (std::vector<SoftBodyNode*>::iterator it = mSoftBodyNodes.begin();
//       it != mSoftBodyNodes.end(); ++it) {
//    for (int i = 0; i < (*it)->getNumPointMasses(); ++i) {
//      PointMass* pm = (*it)->getPointMass(i);
//      int iStart = pm->getGenCoord(0)->getIndexInSkeleton();
//      mFd.segment<3>(iStart) = -(*it)->getDampingCoefficient() * pm->getGenVels();
//    }
//  }
//}

//==============================================================================
void Skeleton::computeForwardDynamics()
{
  //
  computeForwardDynamicsRecursionPartA();

  //
  computeForwardDynamicsRecursionPartB();
}

//==============================================================================
void Skeleton::computeForwardDynamicsRecursionPartA()
{
  // Update body transformations, velocities, and partial acceleration due to
  // parent joint's velocity
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->updateTransform();
    (*it)->updateVelocity();
    (*it)->updatePartialAcceleration();
  }

  mIsArticulatedInertiaDirty = true;
  mIsMassMatrixDirty = true;
  mIsAugMassMatrixDirty = true;
  mIsInvMassMatrixDirty = true;
  mIsInvAugMassMatrixDirty = true;
  mIsCoriolisVectorDirty = true;
  mIsGravityForceVectorDirty = true;
  mIsCombinedVectorDirty = true;
  mIsExternalForceVectorDirty = true;
//  mIsDampingForceVectorDirty = true;

  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->mIsBodyJacobianDirty = true;
    (*it)->mIsBodyJacobianTimeDerivDirty = true;
  }
}

//==============================================================================
void Skeleton::computeForwardDynamicsRecursionPartB()
{
  // Backward recursion
//  if (mIsArticulatedInertiaDirty)
  {
    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
         it != mBodyNodes.rend(); ++it)
    {
      (*it)->updateArtInertia(mTimeStep);
      (*it)->updateBiasForce(mGravity, mTimeStep);
    }

    mIsArticulatedInertiaDirty = false;
  }
//  else
//  {
//    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
//         it != mBodyNodes.rend(); ++it)
//    {
//      (*it)->updateBiasForce(mGravity, mTimeStep);
//    }
//  }

  // Forward recursion
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->updateJointAndBodyAcceleration();
    (*it)->updateTransmittedForce();
  }
}

//==============================================================================
void Skeleton::computeInverseDynamics(bool _withExternalForces,
                                      bool _withDampingForces)
{
  //
  computeInverseDynamicsRecursionA();

  //
  computeInverseDynamicsRecursionB(_withExternalForces, _withDampingForces);
}

//==============================================================================
void Skeleton::computeInverseDynamicsRecursionA()
{
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->updateTransform();
    (*it)->updateVelocity();
    (*it)->updatePartialAcceleration();
    (*it)->updateAcceleration();
  }

  mIsArticulatedInertiaDirty = true;
  mIsMassMatrixDirty = true;
  mIsAugMassMatrixDirty = true;
  mIsInvMassMatrixDirty = true;
  mIsInvAugMassMatrixDirty = true;
  mIsCoriolisVectorDirty = true;
  mIsGravityForceVectorDirty = true;
  mIsCombinedVectorDirty = true;
  mIsExternalForceVectorDirty = true;
//  mIsDampingForceVectorDirty = true;

  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->mIsBodyJacobianDirty = true;
    (*it)->mIsBodyJacobianTimeDerivDirty = true;
  }
}

//==============================================================================
void Skeleton::computeInverseDynamicsRecursionB(bool _withExternalForces,
                                                bool _withDampingForces)
{
  // Skip immobile or 0-dof skeleton
  if (getDof() == 0)
    return;

  // Backward recursion
  for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
       it != mBodyNodes.rend(); ++it)
  {
    (*it)->updateBodyForce(mGravity, _withExternalForces);
    (*it)->updateGeneralizedForce(_withDampingForces);
  }
}

//==============================================================================
void Skeleton::computeHybridDynamics()
{
  dterr << "Not implemented yet.\n";
}

//==============================================================================
void Skeleton::computeHybridDynamicsRecursionA()
{
  dterr << "Not implemented yet.\n";
}

//==============================================================================
void Skeleton::computeHybridDynamicsRecursionB()
{
  dterr << "Not implemented yet.\n";
}

void Skeleton::clearExternalForces() {
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it) {
    (*it)->clearExternalForces();
  }
}

//==============================================================================
void Skeleton::clearConstraintImpulses()
{
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->clearConstraintImpulse();
  }
}

//==============================================================================
void Skeleton::updateBiasImpulse(BodyNode* _bodyNode)
{
  // Assertions
  assert(_bodyNode != NULL);
  assert(getDof() > 0);

  // This skeleton should contains _bodyNode
  assert(std::find(mBodyNodes.begin(), mBodyNodes.end(), _bodyNode)
         != mBodyNodes.end());

#ifndef NDEBUG
  // All the constraint impulse should be zero
  for (int i = 0; i < mBodyNodes.size(); ++i)
    assert(mBodyNodes[i]->mConstraintImpulse == Eigen::Vector6d::Zero());
#endif

  // Prepare cache data
  BodyNode* it = _bodyNode;
  while (it != NULL)
  {
    it->updateBiasImpulse();
    it = it->getParentBodyNode();
  }
}

//==============================================================================
void Skeleton::updateBiasImpulse(BodyNode* _bodyNode,
                                 const Eigen::Vector6d& _imp)
{
  // Assertions
  assert(_bodyNode != NULL);
  assert(getDof() > 0);

  // This skeleton should contain _bodyNode
  assert(std::find(mBodyNodes.begin(), mBodyNodes.end(), _bodyNode)
         != mBodyNodes.end());

#ifndef NDEBUG
  // All the constraint impulse should be zero
  for (int i = 0; i < mBodyNodes.size(); ++i)
    assert(mBodyNodes[i]->mConstraintImpulse == Eigen::Vector6d::Zero());
#endif

  // Set impulse to _bodyNode
//  Eigen::Vector6d oldConstraintImpulse =_bodyNode->mConstraintImpulse;
  _bodyNode->mConstraintImpulse = _imp;

  // Prepare cache data
  BodyNode* it = _bodyNode;
  while (it != NULL)
  {
    it->updateBiasImpulse();
    it = it->getParentBodyNode();
  }

  // TODO(JS): Do we need to backup and restore the original value?
//  _bodyNode->mConstraintImpulse = oldConstraintImpulse;
  _bodyNode->mConstraintImpulse.setZero();
}

//==============================================================================
void Skeleton::updateBiasImpulse(SoftBodyNode* _softBodyNode,
                                 PointMass* _pointMass,
                                 const Eigen::Vector3d& _imp)
{
  // Assertions
  assert(_softBodyNode != NULL);
  assert(getDof() > 0);

  // This skeleton should contain _bodyNode
  assert(std::find(mSoftBodyNodes.begin(), mSoftBodyNodes.end(), _softBodyNode)
         != mSoftBodyNodes.end());

#ifndef NDEBUG
  // All the constraint impulse should be zero
  for (int i = 0; i < mBodyNodes.size(); ++i)
    assert(mBodyNodes[i]->mConstraintImpulse == Eigen::Vector6d::Zero());
#endif

  // Set impulse to _bodyNode
  Eigen::Vector3d oldConstraintImpulse =_pointMass->getConstraintImpulses();
  _pointMass->setConstraintImpulse(_imp, true);

  // Prepare cache data
  BodyNode* it = _softBodyNode;
  while (it != NULL)
  {
    it->updateBiasImpulse();
    it = it->getParentBodyNode();
  }

  // TODO(JS): Do we need to backup and restore the original value?
  _pointMass->setConstraintImpulses(oldConstraintImpulse);
}

//==============================================================================
void Skeleton::updateVelocityChange()
{
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->updateJointVelocityChange();
  }
}

//==============================================================================
void Skeleton::setImpulseApplied(bool _val)
{
  mIsImpulseApplied = _val;
}

//==============================================================================
bool Skeleton::isImpulseApplied() const
{
  return mIsImpulseApplied;
}

//==============================================================================
void Skeleton::computeImpulseForwardDynamics()
{
  // Skip immobile or 0-dof skeleton
  if (!isMobile() || getDof() == 0)
    return;

  // Backward recursion
  if (mIsArticulatedInertiaDirty)
  {
    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
         it != mBodyNodes.rend(); ++it)
    {
      (*it)->updateArtInertia(mTimeStep);
      (*it)->updateBiasImpulse();
    }

    mIsArticulatedInertiaDirty = false;
  }
  else
  {
    for (std::vector<BodyNode*>::reverse_iterator it = mBodyNodes.rbegin();
         it != mBodyNodes.rend(); ++it)
    {
      (*it)->updateBiasImpulse();
    }
  }

  // Forward recursion
  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    (*it)->updateJointVelocityChange();
//    (*it)->updateBodyVelocityChange();
    (*it)->updateBodyImpForceFwdDyn();
  }

  for (std::vector<BodyNode*>::iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    // 1. dq = dq + del_dq
    // 2. ddq = ddq + del_dq / dt
    // 3. tau = tau + imp / dt
    (*it)->updateConstrainedJointAndBodyAcceleration(mTimeStep);

    // 4. F(+) = F(-) + ImpF / dt
    (*it)->updateConstrainedTransmittedForce(mTimeStep);
  }
}

void Skeleton::setInternalForceVector(const Eigen::VectorXd& _forces) {
  setGenForces(_forces);
}

void Skeleton::setMinInternalForceVector(const Eigen::VectorXd& _minForces) {
  setGenForcesMin(_minForces);
}

Eigen::VectorXd Skeleton::getMinInternalForces() const {
  return getGenForcesMin();
}

void Skeleton::setMaxInternalForceVector(const Eigen::VectorXd& _maxForces) {
  setGenForcesMax(_maxForces);
}

Eigen::VectorXd Skeleton::getMaxInternalForceVector() const {
  return getGenForcesMax();
}

void Skeleton::clearInternalForces() {
  setGenForces(Eigen::VectorXd::Zero(getDof()));
}

void Skeleton::setConstraintForceVector(const Eigen::VectorXd& _Fc) {
  mFc = _Fc;
}

Eigen::Vector3d Skeleton::getWorldCOM() {
  // COM
  Eigen::Vector3d com(0.0, 0.0, 0.0);

  // Compute sum of each body's COM multiplied by body's mass
  const int nNodes = getNumBodyNodes();
  for (int i = 0; i < nNodes; i++) {
    BodyNode* bodyNode = getBodyNode(i);
    com += bodyNode->getMass() * bodyNode->getWorldCOM();
  }

  // Divide the sum by the total mass
  assert(mTotalMass != 0.0);
  return com / mTotalMass;
}

Eigen::Vector3d Skeleton::getWorldCOMVelocity() {
  // Velocity of COM
  Eigen::Vector3d comVel(0.0, 0.0, 0.0);

  // Compute sum of each body's COM velocities multiplied by body's mass
  const int nNodes = getNumBodyNodes();
  for (int i = 0; i < nNodes; i++) {
    BodyNode* bodyNode = getBodyNode(i);
    comVel += bodyNode->getMass() * bodyNode->getWorldCOMVelocity();
  }

  // Divide the sum by the total mass
  assert(mTotalMass != 0.0);
  return comVel / mTotalMass;
}

Eigen::Vector3d Skeleton::getWorldCOMAcceleration() {
  // Acceleration of COM
  Eigen::Vector3d comAcc(0.0, 0.0, 0.0);

  // Compute sum of each body's COM accelerations multiplied by body's mass
  const int nNodes = getNumBodyNodes();
  for (int i = 0; i < nNodes; i++) {
    BodyNode* bodyNode = getBodyNode(i);
    comAcc += bodyNode->getMass() * bodyNode->getWorldCOMAcceleration();
  }

  // Divide the sum by the total mass
  assert(mTotalMass != 0.0);
  return comAcc / mTotalMass;
}

Eigen::MatrixXd Skeleton::getWorldCOMJacobian() {
  // Jacobian of COM
  Eigen::MatrixXd J = Eigen::MatrixXd::Zero(3, getDof());

  // Compute sum of each body's Jacobian of COM accelerations multiplied by
  // body's mass
  const int nNodes = getNumBodyNodes();
  for (int i = 0; i < nNodes; i++) {
    // BodyNode iterator
    BodyNode* bodyNode = getBodyNode(i);

    // Compute weighted Jacobian
    Eigen::MatrixXd localJ
        = bodyNode->getMass()
          * bodyNode->getWorldJacobian(
              bodyNode->getLocalCOM(), true).bottomRows<3>();

    // Assign the weighted Jacobian to total Jacobian
    for (int j = 0; j < bodyNode->getNumDependentGenCoords(); ++j) {
      int idx = bodyNode->getDependentGenCoordIndex(j);
      J.col(idx) += localJ.col(j);
    }
  }

  // Divide the sum by the total mass
  assert(mTotalMass != 0.0);
  return J / mTotalMass;
}

Eigen::MatrixXd Skeleton::getWorldCOMJacobianTimeDeriv() {
  // Jacobian time derivative of COM
  Eigen::MatrixXd dJ = Eigen::MatrixXd::Zero(3, getDof());

  // Compute sum of each body's Jacobian time derivative of COM accelerations
  // multiplied by body's mass
  const int nNodes = getNumBodyNodes();
  for (int i = 0; i < nNodes; i++) {
    // BodyNode iterator
    BodyNode* bodyNode = getBodyNode(i);

    // Compute weighted Jacobian time derivative
    Eigen::MatrixXd localJ
        = bodyNode->getMass()
          * bodyNode->getWorldJacobianTimeDeriv(bodyNode->getLocalCOM(), true).bottomRows<3>();

    // Assign the weighted Jacobian to total Jacobian time derivative
    for (int j = 0; j < bodyNode->getNumDependentGenCoords(); ++j) {
      int idx = bodyNode->getDependentGenCoordIndex(j);
      dJ.col(idx) += localJ.col(j);
    }
  }

  // Divide the sum by the total mass
  assert(mTotalMass != 0.0);
  return dJ / mTotalMass;
}

double Skeleton::getKineticEnergy() const {
  double KE = 0.0;

  for (std::vector<BodyNode*>::const_iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    KE += (*it)->getKineticEnergy();
  }

  assert(KE >= 0.0 && "Kinetic energy should be positive value.");
  return KE;
}

double Skeleton::getPotentialEnergy() const {
  double PE = 0.0;

  for (std::vector<BodyNode*>::const_iterator it = mBodyNodes.begin();
       it != mBodyNodes.end(); ++it)
  {
    PE += (*it)->getPotentialEnergy(mGravity);
    PE += (*it)->getParentJoint()->getPotentialEnergy();
  }

  return PE;
}

}  // namespace dynamics
}  // namespace dart
