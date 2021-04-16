// Copyright 1996-2021 Cyberbotics Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "WbWorld.hpp"

#include "WbApplication.hpp"
#include "WbBackground.hpp"
#include "WbBallJointParameters.hpp"
#include "WbBasicJoint.hpp"
#include "WbField.hpp"
#include "WbFileUtil.hpp"
#include "WbGeometry.hpp"
#include "WbGroup.hpp"
#include "WbHingeJointParameters.hpp"
#include "WbImageTexture.hpp"
#include "WbJoint.hpp"
#include "WbJointDevice.hpp"
#include "WbJointParameters.hpp"
#include "WbLed.hpp"
#include "WbLog.hpp"
#include "WbMFNode.hpp"
#include "WbMFString.hpp"
#include "WbMotor.hpp"
#include "WbNodeOperations.hpp"
#include "WbNodeReader.hpp"
#include "WbNodeUtilities.hpp"
#include "WbOdeContact.hpp"
#include "WbPbrAppearance.hpp"
#include "WbPerspective.hpp"
#include "WbPreferences.hpp"
#include "WbProject.hpp"
#include "WbPropeller.hpp"
#include "WbProtoList.hpp"
#include "WbProtoModel.hpp"
#include "WbRenderingDevice.hpp"
#include "WbRobot.hpp"
#include "WbSimulationState.hpp"
#include "WbSlot.hpp"
#include "WbSolid.hpp"
#include "WbStandardPaths.hpp"
#include "WbTemplateManager.hpp"
#include "WbTokenizer.hpp"
#include "WbViewpoint.hpp"
#include "WbVrmlWriter.hpp"
#include "WbWorldInfo.hpp"
#include "WbWrenOpenGlContext.hpp"
#include "WbWrenRenderingContext.hpp"

#include <wren/scene.h>

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringListIterator>
#include <QtCore/QTextStream>

#include <ode/fluid_dynamics/ode_fluid_dynamics.h>

#include <cassert>

static WbWorld *gInstance = NULL;
bool WbWorld::cX3DMetaFileExport = false;
bool WbWorld::cX3DStreaming = false;

WbWorld *WbWorld::instance() {
  return gInstance;
}

WbWorld::WbWorld(WbProtoList *protos, WbTokenizer *tokenizer) :
  mWorldLoadingCanceled(false),
  mResetRequested(false),
  mRestartControllers(false),
  mIsModified(false),
  mIsModifiedFromSceneTree(false),
  mWorldInfo(NULL),
  mViewpoint(NULL),
  mPerspective(NULL),
  mProtos(protos ? protos : new WbProtoList()),
  mLastAwakeningTime(0.0),
  mIsLoading(false),
  mIsCleaning(false),
  mIsVideoRecording(false) {
  gInstance = this;
  WbNode::setInstantiateMode(true);
  WbNode::setGlobalParentNode(NULL);
  mRoot = new WbGroup();
  mRoot->setUniqueId(0);
  WbNode::setGlobalParentNode(mRoot);
  mRadarTargets.clear();
  mCameraRecognitionObjects.clear();

  if (tokenizer) {
    mFileName = tokenizer->fileName();
    if (mFileName == (WbStandardPaths::emptyProjectPath() + "worlds/" + WbProject::newWorldFileName()))
      mFileName = WbStandardPaths::unnamedWorld();

    mPerspective = new WbPerspective(mFileName);
    mPerspective->load();

    // read/create nodes
    WbNodeReader reader;
    WbApplication::instance()->setWorldLoadingStatus(tr("Parsing nodes"));
    connect(&reader, &WbNodeReader::readNodesHasProgressed, WbApplication::instance(), &WbApplication::setWorldLoadingProgress);
    connect(WbApplication::instance(), &WbApplication::worldLoadingWasCanceled, &reader, &WbNodeReader::cancelReadNodes);
    QList<WbNode *> nodes = reader.readNodes(tokenizer, mFileName);
    disconnect(WbApplication::instance(), &WbApplication::worldLoadingWasCanceled, &reader, &WbNodeReader::cancelReadNodes);
    disconnect(&reader, &WbNodeReader::readNodesHasProgressed, WbApplication::instance(),
               &WbApplication::setWorldLoadingProgress);
    if (WbApplication::instance()->wasWorldLoadingCanceled()) {
      mWorldLoadingCanceled = true;
      return;
    }
    WbTemplateManager::instance()->blockRegeneration(true);
    WbField *childrenField = mRoot->findField("children");
    int index = 0;
    WbApplication::instance()->setWorldLoadingStatus(tr("Creating nodes"));
    foreach (WbNode *node, nodes) {
      index++;
      WbApplication::instance()->setWorldLoadingProgress(index * 100 / nodes.size());
      if (WbApplication::instance()->wasWorldLoadingCanceled()) {
        mWorldLoadingCanceled = true;
        return;
      }
      QString errorMessage;
      if (WbNodeUtilities::isAllowedToInsert(childrenField, node->nodeModelName(), mRoot, errorMessage, WbNode::STRUCTURE_USE,
                                             WbNodeUtilities::slotType(node), QStringList(node->nodeModelName()))) {
        node->validate();
        mRoot->addChild(node);
      } else
        mRoot->parsingWarn(errorMessage);
    }
    WbTemplateManager::instance()->blockRegeneration(false);

    // ensure a minimal set of nodes for a functional world
    checkPresenceOfMandatoryNodes();
  } else {
    mFileName = WbStandardPaths::unnamedWorld();

    mPerspective = new WbPerspective(mFileName);
    mPerspective->load();

    // create default nodes
    mWorldInfo = new WbWorldInfo();
    mViewpoint = new WbViewpoint();
    mRoot->addChild(mWorldInfo);
    mRoot->addChild(mViewpoint);
  }

  WbNode::setGlobalParentNode(NULL);
  updateTopLevelLists();

  // world loading stuff
  connect(root(), &WbGroup::childFinalizationHasProgressed, WbApplication::instance(), &WbApplication::setWorldLoadingProgress);
  connect(this, &WbWorld::worldLoadingStatusHasChanged, WbApplication::instance(), &WbApplication::setWorldLoadingStatus);
  connect(this, &WbWorld::worldLoadingHasProgressed, WbApplication::instance(), &WbApplication::setWorldLoadingProgress);
  connect(WbApplication::instance(), &WbApplication::worldLoadingWasCanceled, root(), &WbGroup::cancelFinalization);
}

void WbWorld::finalize() {
  disconnect(WbApplication::instance(), &WbApplication::worldLoadingWasCanceled, root(), &WbGroup::cancelFinalization);
  disconnect(this, &WbWorld::worldLoadingStatusHasChanged, WbApplication::instance(), &WbApplication::setWorldLoadingStatus);
  disconnect(this, &WbWorld::worldLoadingHasProgressed, WbApplication::instance(), &WbApplication::setWorldLoadingProgress);
  disconnect(root(), &WbGroup::childFinalizationHasProgressed, WbApplication::instance(),
             &WbApplication::setWorldLoadingProgress);
  if (WbApplication::instance()->wasWorldLoadingCanceled())
    mWorldLoadingCanceled = true;

  connect(mRoot, &WbGroup::topLevelListsUpdateRequested, this, &WbWorld::updateTopLevelLists);
  connect(mWorldInfo, &WbWorldInfo::globalPhysicsPropertiesChanged, this, &WbWorld::awake);
  connect(WbNodeOperations::instance(), &WbNodeOperations::nodeAdded, this, &WbWorld::storeAddedNodeIfNeeded);

  if (WbProject::current())
    connect(WbProject::current(), &WbProject::pathChanged, this, &WbWorld::updateProjectPath);

  // check for Solid name clash
  QSet<const QString> topSolidNameSet;
  foreach (WbSolid *s, mTopSolids)
    s->resolveNameClashIfNeeded(false, true, mTopSolids, &topSolidNameSet);

  // simplify node structure, if possible
  collapseNestedProtos();
}

bool WbWorld::isParameterNodeChainCollapsable(WbNode *node, int depth) {
  if (node && node->protoParameterNode())
    isParameterNodeChainCollapsable(node->protoParameterNode(), depth + 1);

  if (depth == 0 || WbNodeUtilities::isVisible(node))
    return false;
  else
    return true;
}

bool WbWorld::isProtoParameterNodeChainCollapsable(WbNode *node) {
  // it's sufficient for the top of the chain (which is a protoParameterNode that has no other protoParameterNode links)
  // not to be visible for it to be collapsable
  const WbNode *parent = node->parentNode();
  assert(node && parent);
  if (!WbNodeUtilities::isVisible(node) && node->isProtoParameterNode() &&
      node->protoParameterNode() == NULL) {  // && parent->isNestedProtoNode()
    // check if any of the fields themselves are visible
    QVector<WbField *> fields = node->fields();
    for (int i = 0; i < fields.size(); ++i)
      if (WbNodeUtilities::isVisible(fields[i])) {
        printf("!!! field %s (%p) IS VISIBLE, skipping %s\n", fields[i]->name().toUtf8().constData(), fields[i],
               node->usefulName().toUtf8().constData());
        return false;
      }
    return true;
  } else
    return false;
}

/*
void WbWorld::recursiveAliasUnlink(WbNode *node, int depth) {
  if (!node)
    return;

  if (depth == 0) {  // nothing to remove in the first level
    recursiveAliasUnlink(node->protoParameterNode(), depth + 1);
    return;
  }

  if (node->isProtoParameterNode()) {
    // remove reference from internal nodes
    QVector<WbNode *> parameterInstances = node->protoParameterNodeInstances();
    for (int j = 0; j < parameterInstances.size(); ++j) {
      printf("node %s (%p) is an instance of node %s (%p)\n", parameterInstances[j]->usefulName().toUtf8().constData(),
             parameterInstances[j], node->usefulName().toUtf8().constData(), node);
      parameterInstances[j]->unlinkProtoParameter();
      parameterInstances[j]->clearRefProtoParameterNode();
    }
    node->clearRefProtoParameterNodeInstances();
  }

  // take care of references at the nested proto level
  if (node->isNestedProtoNode()) {
    QVector<WbField *> fieldsList = node->fields();
    for (int j = 0; j < fieldsList.size(); ++j)
      fieldsList[j]->setParameter(NULL);
  }

  recursiveAliasUnlink(node->protoParameterNode(), depth + 1);
}
*/

void WbWorld::recursiveAliasUnlink(WbNode *currentNode, WbNode *previousNode, int depth) {
  printf("recursiveAliasUnlink\n");
  if (!currentNode)
    return;

  /*
  // needed ????
  if (depth == 0) {  // parent node of the first protoParameterNode is the nested proto node
    const WbNode *parent = node->parentNode();
    assert(parent->isNestedProtoNode());
    // clear references at the level of the nested proto
    QVector<WbField *> fields = parent->fields();
    printf("%s\n", parent->usefulName().toUtf8().constData());
    for (int i = 0; i < fields.size(); ++i) {
      printf("> field %s set to NULL\n", fields[i]->name().toUtf8().constData());
      fields[i]->setParameter(NULL);
    }
  }
  */

  // go to the bottom of the chain
  QVector<WbNode *> instances = currentNode->protoParameterNodeInstances();
  for (int i = instances.size() - 1; i >= 0; --i) {
    recursiveAliasUnlink(instances[i], currentNode, depth + 1);
  }

  // NB: order matters, reach this point only when unwinding the recursion

  if (!previousNode)  // it's NULL if it isn't a chain or if it finished unlinking
    return;

  QVector<WbField *> currentFields = currentNode->fields();
  QVector<WbField *> previousFields = previousNode->fields();

  assert(currentNode->model() == previousNode->model() && currentFields.size() == previousFields.size());

  // swap current fields of this instance with the contents of the previous one. This is necessary if one of the fields is
  // exposed (e.g. a single field of a SFNode since it doesn't create a full node for it, as is the case for an exposed SFNode,
  // but just a field)

  printf("%s\n", currentNode->usefulName().toUtf8().constData());
  for (int i = 0; i < currentFields.size(); ++i) {
    printf(" field %s set from %p to %p\n", currentFields[i]->name().toUtf8().constData(), currentFields[i]->parameter(),
           previousFields[i]->parameter());
    currentFields[i]->setParameter(previousFields[i]->parameter());
  }

  // remove ProtoParameterNode pointer
  printf("(%d) %s setting mProtoParameterNode from %p to NULL\n", depth, currentNode->usefulName().toUtf8().constData(),
         currentNode->protoParameterNode());
  currentNode->setProtoParameterNode(NULL);
}

void recursiveAliasCollapser(WbNode *node) {
  QList<WbNode *> subNodes = node->subNodes(true, true, true);

  printf("%s %d subnodes\n", node->usefulName().toUtf8().constData(), subNodes.size());
  for (int j = subNodes.size() - 1; j >= 0; --j) {
    // printf("  %s (%p) --> alias (%p)\n", subNodes[j]->usefulName().toUtf8().constData(), subNodes[j]);
  }
}

void WbWorld::printInstances(WbNode *node, int depth) {
  QString indent = "";
  for (int i = 0; i < depth; ++i) {
    indent += "  ";
  }
  QVector<WbNode *> instances = node->protoParameterNodeInstances();
  printf("%s%p (%s) has %d instances:\n", indent.toUtf8().constData(), node, node->usefulName().toUtf8().constData(),
         instances.size());
  // indent += "  ";
  if (instances.size() == 0)
    printf("%sNULL\n", indent.toUtf8().constData());
  for (int i = 0; i < instances.size(); ++i) {
    printf("%s%p (%s)\n", indent.toUtf8().constData(), instances[i], instances[i]->usefulName().toUtf8().constData());
    printInstances(instances[i], depth + 1);
  }
}

void WbWorld::collapseNestedProtos() {
  printf("=============================\n");
  mRoot->printDebugNodeStructure();
  printf("=============================\n");

  QList<WbNode *> nodes = mRoot->subNodes(true, true, true);
  for (int i = 0; i < nodes.size(); ++i) {
    printf("[%2d]NODE: %s (%p) :: %d \n", i, nodes[i]->usefulName().toUtf8().constData(), nodes[i],
           nodes[i]->isProtoParameterNode());
    printf("    PARAMETER NODE: %p\n", nodes[i]->protoParameterNode());
  }
  for (int i = 0; i < nodes.size(); ++i) {
    nodes[i]->printFieldsAndParams();
  }
  printf("\nNODE FLAGS\n\n");
  for (int i = 0; i < nodes.size(); ++i) {
    printf("%d) %50s :  %d, %d, %d, %d | %d %d \n", i, nodes[i]->usefulName().toUtf8().constData(),
           WbNodeUtilities::isVisible(nodes[i]), nodes[i]->isProtoParameterNode(), nodes[i]->isNestedProtoNode(),
           nodes[i]->protoParameterNode() != NULL, isParameterNodeChainCollapsable(nodes[i]),
           isProtoParameterNodeChainCollapsable(nodes[i]));
  }

  printf("\nINSTANCE CHAINS\n");
  for (int i = 0; i < nodes.size(); ++i) {
    printInstances(nodes[i]);
  }

  printf("\nVISIBILITY\n\n");
  for (int i = 0; i < nodes.size(); ++i) {
    printf("%s visibility: %d\n", nodes[i]->usefulName().toUtf8().constData(), WbNodeUtilities::isVisible(nodes[i]));
    QVector<WbField *> parameterList = nodes[i]->fieldsOrParameters();
    for (int j = 0; j < parameterList.size(); ++j) {
      printf("  %s visibility: %d\n", parameterList[j]->name().toUtf8().constData(),
             WbNodeUtilities::isVisible(parameterList[j]));
    }
  }

  printf("\n\n>>>> BEGIN COLLAPSE "
         "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"
         "<<<<<<<<<<<<<<<\n\n");

  QVector<WbNode *> collapsableNodes;
  QVector<WbNode *> protoParameterNodes;
  for (int i = 0; i < nodes.size(); ++i) {
    if (isProtoParameterNodeChainCollapsable(nodes[i]))
      collapsableNodes.append(nodes[i]);
    // if (nodes[i]->protoParameterNode() != NULL)
    if (collapsableNodes.contains(nodes[i]->protoParameterNode()))
      protoParameterNodes.append(nodes[i]);
  }

  printf("COLLAPSABLE\n");
  for (int i = 0; i < collapsableNodes.size(); ++i) {
    printf("  %s (%p, alias is %p)\n", collapsableNodes[i]->usefulName().toUtf8().constData(), collapsableNodes[i],
           collapsableNodes[i]->protoParameterNode());
  }

  printf("PPNs:\n");
  for (int i = 0; i < protoParameterNodes.size(); ++i) {
    printf("  %s (%p, alias is %p)\n", protoParameterNodes[i]->usefulName().toUtf8().constData(), protoParameterNodes[i],
           protoParameterNodes[i]->protoParameterNode());
  }

  printf("INTERNAL\n");
  for (int i = 0; i < collapsableNodes.size(); ++i) {
    QVector<WbField *> f = collapsableNodes[i]->fields();
    printf("# for node %s (%p)\n", collapsableNodes[i]->usefulName().toUtf8().constData(), collapsableNodes[i]);
    for (int j = 0; j < f.size(); ++j) {
      printf("## checking field %s (%p)\n", f[j]->name().toUtf8().constData(), f[j]);
      f[j]->printInternalFields();
    }
  }

  printf("BEGIN SWAP\n");
  for (int i = 0; i < protoParameterNodes.size(); ++i) {
    int ixCollapsable = collapsableNodes.indexOf(protoParameterNodes[i]->protoParameterNode());

    assert(ixCollapsable != -1 && protoParameterNodes[i]->model() == collapsableNodes[ixCollapsable]->model());

    QVector<WbField *> internalFields = protoParameterNodes[i]->fields();
    QVector<WbField *> exposedFields = collapsableNodes[ixCollapsable]->fields();

    assert(internalFields.size() == exposedFields.size());

    // swap current fields of this instance with the contents of the previous one. This is necessary if one of the fields is
    // exposed (e.g. a single field of a SFNode since it doesn't create a full node for it, as is the case for an exposed
    // SFNode, but just a field)
    printf("Swapping aliases for node %s :\n", protoParameterNodes[i]->usefulName().toUtf8().constData());
    for (int j = 0; j < internalFields.size(); ++j) {
      printf(" - field %s (%p -> %p / %p)", internalFields[j]->name().toUtf8().constData(), internalFields[j],
             internalFields[j]->parameter(), exposedFields[j]->parameter());
      internalFields[j]->setParameter(exposedFields[j]->parameter());
      // exposedFields[j]->setParameter(NULL);
      printf(" becomes (%p -> %p)\n", internalFields[j], internalFields[j]->parameter());
    }

    // clear internal field references first otherwise when deleting the protoParameterNode they're set to NULL which
    // would nullify the alias collapsing for visible fields (single fields, not the full SFNode)
    // if (i < 5)
    // exposedFields[i]->clearInternalFields();

    // break the mProtoParameterNode link
    printf("> setting mProtoParameterNode from %p to NULL for %s\n", protoParameterNodes[i]->protoParameterNode(),
           protoParameterNodes[i]->usefulName().toUtf8().constData());
    protoParameterNodes[i]->setProtoParameterNode(NULL);
  }
  printf("END SWAP\n");
  printf("BEGIN CLEAR INTERNAL\n");
  for (int i = collapsableNodes.size() - 1; i >= 0; --i) {
    QVector<WbField *> exposedFields = collapsableNodes[i]->fields();
    for (int j = exposedFields.size() - 1; j >= 0; --j) {
      exposedFields[j]->clearInternalFields();
    }
  }
  printf("END CLEAR INTERNAL\n");
  printf("BEGIN DELETE\n");

  // delete
  for (int i = collapsableNodes.size() - 1; i >= 0; --i) {
    WbNode *parent = collapsableNodes[i]->parentNode();  // make const
    if (parent && parent->isNestedProtoNode()) {
      printf(">>>>> deleting %s (%p), its parent is %s (%p)\n", collapsableNodes[i]->usefulName().toUtf8().constData(),
             collapsableNodes[i], parent->usefulName().toUtf8().constData(), parent);

      QVector<WbField *> nestedNodeFields = parent->fields();
      for (int j = nestedNodeFields.size() - 1; j >= 0; --j) {
        if (nestedNodeFields[j]->type() == WB_SF_NODE) {
          printf("field %s is SF_NODE (%p)\n", nestedNodeFields[j]->name().toUtf8().constData(), nestedNodeFields[j]);

          /*
          WbNode *n = dynamic_cast<WbSFNode *>(fields[j]->value())->value();
          if (n) {
            printf("  cast as node gives %s (%p)\n", n->usefulName().toUtf8().constData(), n);
          }
          */
        } else if (nestedNodeFields[j]->type() == WB_MF_NODE) {
          printf("field %s is MF_NODE (%p)\n", nestedNodeFields[j]->name().toUtf8().constData(), nestedNodeFields[j]);
          /*
          WbMFNode *mfnode = dynamic_cast<WbMFNode *>(nestedNodeFields[j]->value());
          for (int i = 0; i < mfnode->size(); ++i) {
            WbNode *n = mfnode->item(i);
            if (n) {
              printf("  cast as node gives %s (%p)\n", n->usefulName().toUtf8().constData(), n);
            }
          }
          */
        }
      }

      QVector<WbField *> nestedNodeParameters = parent->parameters();
      for (int j = nestedNodeParameters.size() - 1; j >= 0; --j) {
        if (nestedNodeParameters[j]->type() == WB_SF_NODE) {
          printf("parameters %s is SF_NODE (%p)\n", nestedNodeParameters[j]->name().toUtf8().constData(),
                 nestedNodeParameters[j]);

          WbNode *n = dynamic_cast<WbSFNode *>(nestedNodeParameters[j]->value())->value();
          if (n && n == collapsableNodes[i]) {
            printf("  cast as node gives %s (%p) (MATCH!)\n", n->usefulName().toUtf8().constData(), n);
            // todel.append(parameters[j]);
            delete nestedNodeParameters[j];
            parent->removeFromParameters(nestedNodeParameters[j]);
          }
        } else if (nestedNodeParameters[j]->type() == WB_MF_NODE) {
          printf("parameters %s is MF_NODE (%p)\n", nestedNodeParameters[j]->name().toUtf8().constData(),
                 nestedNodeParameters[j]);
          WbMFNode *mfnode = dynamic_cast<WbMFNode *>(nestedNodeParameters[j]->value());
          for (int k = mfnode->size() - 1; k >= 0; --k) {
            WbNode *n = mfnode->item(k);
            if (n && n == collapsableNodes[i]) {
              printf("  cast as node gives %s (%p)  (MATCH!)\n", n->usefulName().toUtf8().constData(), n);
              delete nestedNodeParameters[j];
              parent->removeFromParameters(nestedNodeParameters[j]);
            }
          }
        }
      }
    }
  }
  printf("END DELETE\n");

  // delete todel[0];

  /*
  // -- 2nd method
  for (int i = nodes.size() - 1; i >= 0; --i) {
    if (isProtoParameterNodeChainCollapsable(nodes[i])) {
      printf("BEGIN UNLINKING FOR %s\n", nodes[i]->usefulName().toUtf8().constData());
      recursiveAliasUnlink(nodes[i]);
      printf("done recursiveAliasUnlink\n");

      WbNode *parent = nodes[i]->parentNode();  // is a nested proto node
      assert(parent->isNestedProtoNode());

      QVector<WbField *> parameters = parent->parameters();
      bool FND = false;
      for (int j = 0; j < parameters.size(); ++j) {
        // find unlinked parameter and delete it
        if (parameters[j]->parameter() == nodes[i]) {
          FND = true;
          printf("node %s deleting parameter %s (%p)\n", parent->usefulName().toUtf8().constData(),
                 parameters[j]->name().toUtf8().constData(), parameters[j]);
          delete parameters[j];
          parent->removeFromParameters(parameters[j]);
        }
      }
      if (!FND)
        printf("DIDNT FND %s!!!!\n", parameters[i]->name().toUtf8().constData());
    }
  }
  // -- 2nd method end
  */

  // -- previous method --
  /*
  nodes = mRoot->subNodes(true, true, true);
  for (int i = nodes.size() - 1; i >= 0; --i) {
    // take care of nodes instantiated at the PROTO parameter level
    if (nodes[i]->isProtoParameterNode()) {
      // remove reference from internal nodes
      QVector<WbNode *> parameterInstances = nodes[i]->protoParameterNodeInstances();
      for (int j = 0; j < parameterInstances.size(); ++j) {
        // printf("node %s (%p) is an instance of node %s (%p)\n", parameterInstances[j]->usefulName().toUtf8().constData(),
        //       parameterInstances[j], nodes[i]->usefulName().toUtf8().constData(), nodes[i]);
        // parameterInstances[j]->unlinkProtoParameter();
        // parameterInstances[j]->clearRefProtoParameterNode();
      }
      // nodes[i]->clearRefProtoParameterNodeInstances();
    }

    // take care of references at the nested proto level
    if (nodes[i]->isNestedProtoNode()) {
      QVector<WbField *> fieldsList = nodes[i]->fields();
      for (int j = 0; j < fieldsList.size(); ++j)
        ;  // fieldsList[j]->setParameter(NULL);
    }
  }
  // now that the cross references have been removed, we can delete
  for (int i = nodes.size() - 1; i >= 0; --i) {
    if (nodes[i]->isNestedProtoNode()) {
      QVector<WbField *> parametersList = nodes[i]->fieldsOrParameters();
      for (int j = parametersList.size() - 1; j >= 0; --j) {
        // delete parametersList[j];
        // nodes[i]->removeFromParameters(parametersList[j]);
      }
    }
  }
  */

  // -- previous method --

  nodes = mRoot->subNodes(true, true, true);
  printf("\n>>>> END COLLAPSE "
         "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"
         "<<<<<<<<<<<<<<<<<<<<\n\n");

  for (int i = 0; i < nodes.size(); ++i) {
    printf("[%2d]NODE: %s (%p) :: %d \n", i, nodes[i]->usefulName().toUtf8().constData(), nodes[i],
           nodes[i]->isProtoParameterNode());
    printf("    PARAMETER NODE: %p\n", nodes[i]->protoParameterNode());
  }

  for (int i = 0; i < nodes.size(); ++i) {
    nodes[i]->printFieldsAndParams();
  }

  printf("\nNODE FLAGS\n\n");
  for (int i = 0; i < nodes.size(); ++i) {
    printf("%d) %50s :  %d, %d, %d, %d | %d %d \n", i, nodes[i]->usefulName().toUtf8().constData(),
           WbNodeUtilities::isVisible(nodes[i]), nodes[i]->isProtoParameterNode(), nodes[i]->isNestedProtoNode(),
           nodes[i]->protoParameterNode() != NULL, isParameterNodeChainCollapsable(nodes[i]),
           isProtoParameterNodeChainCollapsable(nodes[i]));
  }

  printf("\nINSTANCE CHAINS\n");
  for (int i = 0; i < nodes.size(); ++i) {
    printInstances(nodes[i]);
  }

  printf("=============================\n");
  mRoot->printDebugNodeStructure();
  printf("=============================\n");
}

WbWorld::~WbWorld() {
  delete mRoot;
  delete mProtos;
  WbNode::cleanup();
  gInstance = NULL;

  delete mPerspective;

  WbWrenOpenGlContext::makeWrenCurrent();
  // Sanity-check: make sure only the root wren::Transform remains
  assert(wr_scene_compute_node_count(wr_scene_get_instance()) == 1);
  wr_scene_reset(wr_scene_get_instance());
  WbWrenOpenGlContext::doneWren();
}

bool WbWorld::needSaving() const {
  if (mIsModifiedFromSceneTree)
    return true;
  if (WbPreferences::instance()->value("General/disableSaveWarning").toBool())
    return false;
  else
    return mIsModified;
}

void WbWorld::setModifiedFromSceneTree() {
  if (!mIsModifiedFromSceneTree) {
    mIsModifiedFromSceneTree = true;
    setModified();
  }
}

void WbWorld::setModified(bool isModified) {
  if (mIsModified != isModified) {
    mIsModified = isModified;
    emit modificationChanged(isModified);
  }
}

bool WbWorld::isUnnamed() const {
  return mFileName == WbStandardPaths::unnamedWorld();
}

bool WbWorld::saveAs(const QString &fileName) {
  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly))
    return false;

  WbVrmlWriter writer(&file, fileName);
  writer.writeHeader(fileName);

  const int count = mRoot->childCount();
  for (int i = 0; i < count; ++i) {
    mRoot->child(i)->write(writer);
    writer << "\n";
  }

  writer.writeFooter();

  mFileName = fileName;
  bool isValidProject = true;
  const QString newProjectPath = WbProject::projectPathFromWorldFile(mFileName, isValidProject);
  if (newProjectPath != WbProject::current()->path()) {
    // reset list of loaded and available PROTO nodes
    delete mProtos;
    mProtos = new WbProtoList(isValidProject ? newProjectPath + "protos" : "");
    WbProject::current()->setPath(newProjectPath);
  }

  mIsModified = false;
  mIsModifiedFromSceneTree = false;
  emit modificationChanged(false);

  storeLastSaveTime();

  mRoot->save("__init__");
  return true;
}

bool WbWorld::save() {
  return saveAs(mFileName);
}

bool WbWorld::exportAsHtml(const QString &fileName, bool animation) const {
  assert(fileName.endsWith(".html", Qt::CaseInsensitive));

  WbSimulationState *simulationState = WbSimulationState::instance();
  simulationState->pauseSimulation();

  QString x3dFilename = fileName;
  x3dFilename.replace(QRegExp(".html$", Qt::CaseInsensitive), ".x3d");

  bool success = true;
  QFileInfo fo(fileName);
  QString targetPath = fo.absolutePath() + "/";

  try {
    success = exportAsVrml(x3dFilename);
    if (!success)
      throw tr("Cannot export the x3d file to '%1'").arg(x3dFilename);

    QString titleString(WbWorld::instance()->worldInfo()->title());
    QString infoString;
    const WbMFString &info = WbWorld::instance()->worldInfo()->info();
    for (int i = 0; i < info.size(); ++i) {
      QString line = info.itemToString(i, WbPrecision::DOUBLE_MAX);
      line.replace(QRegExp("^\""), "");
      line.replace(QRegExp("\"$"), "");
      infoString += line + "\n";
    }

    titleString = titleString.toHtmlEscaped();
    infoString = infoString.toHtmlEscaped();
    infoString.replace("\n", "<br/>");

    QList<QPair<QString, QString>> templateValues;
    templateValues << QPair<QString, QString>("%x3dFilename%", QFileInfo(x3dFilename).fileName());
    QString setAnimation;
    if (animation) {
      QString animationFilename = fileName;
      animationFilename.replace(QRegExp(".html$", Qt::CaseInsensitive), ".json");
      setAnimation = "\n          view.setAnimation(\"" + QFileInfo(animationFilename).fileName() + "\", \"play\", true);";
    }

    templateValues << QPair<QString, QString>("%wwiPath%", WbStandardPaths::resourcesWebPath() + "wwi/");
    templateValues << QPair<QString, QString>("%setAnimation%", setAnimation);
    templateValues << QPair<QString, QString>("%title%", titleString);
    templateValues << QPair<QString, QString>("%description%", infoString);

    if (cX3DMetaFileExport) {
      QString metaFilename = fileName;
      metaFilename.replace(QRegExp(".html$", Qt::CaseInsensitive), ".meta.json");
      createX3DMetaFile(metaFilename);
    }

    success = WbFileUtil::copyAndReplaceString(WbStandardPaths::resourcesWebPath() + "templates/x3d_playback.html", fileName,
                                               templateValues);
    if (!success)
      throw tr("Cannot copy 'x3d_playback.html' to '%1'").arg(fileName);

  } catch (const QString &e) {
    WbLog::error(tr("Cannot export html: '%1'").arg(e), true);
  }

  simulationState->resumeSimulation();
  return success;
}

bool WbWorld::exportAsVrml(const QString &fileName) const {
  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly))
    return false;

  WbVrmlWriter writer(&file, fileName);
  write(writer);

  return true;
}

void WbWorld::write(WbVrmlWriter &writer) const {
  if (writer.isX3d()) {
    // make sure all the meshes data are up-to-date
    // only X3D exporter relies on OpenGL data
    // this is needed for example in minimize and streaming mode because the world is exported before the first main rendering
    WbWrenOpenGlContext::makeWrenCurrent();
    wr_scene_apply_pending_updates(wr_scene_get_instance());
    WbWrenOpenGlContext::doneWren();
  }

  assert(mPerspective);
  QMap<QString, QString> parameters = mPerspective->x3dExportParameters();
  writer.setX3DFrustumCullingValue(parameters.value("frustumCulling"));
  writer.writeHeader(worldInfo()->title());

  // write nodes
  const int count = mRoot->childCount();
  for (int i = 0; i < count; ++i) {
    mRoot->child(i)->write(writer);
    writer << "\n";
  }
  QStringList list;
  const WbMFString &info = worldInfo()->info();
  const int n = info.size();
  for (int i = 0; i < n; ++i)
    list << info.item(i);
  writer.writeFooter(&list);
}

WbNode *WbWorld::findTopLevelNode(const QString &modelName, int preferredPosition) const {
  WbNode *result = NULL;

  WbMFNode::Iterator it(mRoot->children());
  int position = 0;
  while (it.hasNext()) {
    WbNode *const node = it.next();
    if (node->nodeModelName() == modelName) {
      if (result)
        WbLog::warning(tr("'%1': found duplicate %2 node.").arg(mFileName, modelName), false, WbLog::PARSING);
      else {
        result = node;
        if (position != preferredPosition)
          WbLog::warning(tr("'%1': %2 node should be preferably included at position %3 instead of position %4.")
                           .arg(mFileName)
                           .arg(modelName)
                           .arg(preferredPosition + 1)
                           .arg(position + 1),
                         false, WbLog::PARSING);
      }
    }
    ++position;
  }

  if (!result)
    WbLog::warning(tr("'%1': added missing %2 node.").arg(mFileName, modelName), false, WbLog::PARSING);

  return result;
}

void WbWorld::checkPresenceOfMandatoryNodes() {
  mWorldInfo = static_cast<WbWorldInfo *>(findTopLevelNode("WorldInfo", 0));
  if (!mWorldInfo) {
    mWorldInfo = new WbWorldInfo();
    mRoot->insertChild(0, mWorldInfo);
  }

  mViewpoint = static_cast<WbViewpoint *>(findTopLevelNode("Viewpoint", 1));
  if (!mViewpoint) {
    mViewpoint = new WbViewpoint();
    mRoot->insertChild(1, mViewpoint);
  }
}

void WbWorld::createX3DMetaFile(const QString &filename) const {
  QJsonArray robotArray;
  foreach (const WbRobot *robot, mRobots) {  // foreach robot.
    QJsonObject robotObject;
    QJsonArray deviceArray;
    for (int d = 0; d < robot->deviceCount(); ++d) {  // foreach device.
      // Export the device name and type.
      const WbDevice *device = robot->device(d);
      QJsonObject deviceObject;
      deviceObject.insert("name", device->deviceName());
      const WbBaseNode *deviceBaseNode = dynamic_cast<const WbBaseNode *>(device);
      const WbJointDevice *jointDevice = dynamic_cast<const WbJointDevice *>(device);
      const WbMotor *motor = dynamic_cast<const WbMotor *>(jointDevice);

      if (deviceBaseNode)
        deviceObject.insert("type", deviceBaseNode->nodeModelName());

      if (jointDevice && jointDevice->joint()) {  // case: joint devices.
        deviceObject.insert("transformID", QString("n%1").arg(jointDevice->joint()->solidEndPoint()->uniqueId()));
        if (motor) {
          deviceObject.insert("minPosition", motor->minPosition());
          deviceObject.insert("maxPosition", motor->maxPosition());
          deviceObject.insert("position", motor->position());
          const WbJointParameters *jointParameters = NULL;
          if (motor->positionIndex() == 3)
            jointParameters = motor->joint()->parameters3();
          else if (motor->positionIndex() == 2)
            jointParameters = motor->joint()->parameters2();
          else {
            assert(motor->positionIndex() == 1);
            jointParameters = motor->joint()->parameters();
          }
          deviceObject.insert("axis", jointParameters->axis().toString(WbPrecision::FLOAT_MAX));
          const WbBallJointParameters *ballJointParameters = dynamic_cast<const WbBallJointParameters *>(jointParameters);
          const WbHingeJointParameters *hingeJointParameters = dynamic_cast<const WbHingeJointParameters *>(jointParameters);
          if (hingeJointParameters)
            deviceObject.insert("anchor", hingeJointParameters->anchor().toString(WbPrecision::FLOAT_MAX));
          else if (ballJointParameters)
            deviceObject.insert("anchor", ballJointParameters->anchor().toString(WbPrecision::FLOAT_MAX));
          else
            deviceObject.insert("anchor", "0 0 0");
        }
      } else if (jointDevice && jointDevice->propeller() && motor) {  // case: propeller.
        WbSolid *helix = jointDevice->propeller()->helix(WbPropeller::SLOW_HELIX);
        deviceObject.insert("transformID", QString("n%1").arg(helix->uniqueId()));
        deviceObject.insert("position", motor->position());
        deviceObject.insert("axis", motor->propeller()->axis().toString(WbPrecision::FLOAT_MAX));
        deviceObject.insert("minPosition", motor->minPosition());
        deviceObject.insert("maxPosition", motor->maxPosition());
        deviceObject.insert("anchor", "0 0 0");
      } else {  // case: other WbDevice nodes.
        const WbBaseNode *parent =
          jointDevice ? dynamic_cast<const WbBaseNode *>(deviceBaseNode->parentNode()) : deviceBaseNode;
        // Retrieve closest exported Transform parent, and compute its translation offset.
        WbMatrix4 m;
        while (parent) {
          if (parent->shallExport()) {
            deviceObject.insert("transformID", QString("n%1").arg(parent->uniqueId()));
            WbVector3 v = m.translation();
            if (!v.almostEquals(WbVector3()))
              deviceObject.insert("transformOffset", v.toString(WbPrecision::FLOAT_MAX));
            if (motor && parent->nodeType() == WB_NODE_TRACK)
              deviceObject.insert("track", "true");
            break;
          } else {
            const WbAbstractTransform *transform = dynamic_cast<const WbAbstractTransform *>(parent);
            if (transform)
              m *= transform->vrmlMatrix();
          }
          parent = dynamic_cast<const WbBaseNode *>(parent->parentNode());
        }
        // LED case: export color data.
        const WbLed *led = dynamic_cast<const WbLed *>(device);
        if (led) {
          deviceObject.insert("ledGradual", led->isGradual());
          QJsonArray colorArray;
          for (int c = 0; c < led->colorsCount(); ++c)
            colorArray.push_back(led->color(c).toString(WbPrecision::FLOAT_MAX));
          deviceObject.insert("ledColors", colorArray);
          QJsonArray appearanceArray;
          foreach (const WbPbrAppearance *appearance, led->pbrAppearances())
            appearanceArray.push_back(QString("n%1").arg(appearance->uniqueId()));
          deviceObject.insert("ledPBRAppearanceIDs", appearanceArray);
        }
      }
      deviceArray.push_back(deviceObject);
    }
    robotObject.insert("name", robot->name());
    robotObject.insert("robotID", QString("n%1").arg(robot->uniqueId()));
    robotObject.insert("devices", deviceArray);
    robotArray.push_back(robotObject);
  }
  QJsonDocument document(robotArray);
  QFile jsonFile(filename);
  jsonFile.open(QFile::WriteOnly);
  jsonFile.write(document.toJson());
}

WbSolid *WbWorld::findSolid(const QString &name) const {
  WbMFNode::Iterator it(mRoot->children());
  while (it.hasNext()) {
    WbSolid *const solidChild = dynamic_cast<WbSolid *>(it.next());
    if (solidChild) {
      WbSolid *const found = solidChild->findSolid(name);
      if (found)
        return found;
    }
  }
  return NULL;
}

QList<WbSolid *> WbWorld::findSolids(bool visibleNodes) const {
  const QList<WbNode *> &allNodes = mRoot->subNodes(true, !visibleNodes, false);
  QList<WbSolid *> allSolids;

  foreach (WbNode *const node, allNodes) {
    WbSolid *const solid = dynamic_cast<WbSolid *>(node);
    if (solid)
      allSolids.append(solid);
  }

  return allSolids;
}

QStringList WbWorld::listTextureFiles() const {
  QStringList list = mRoot->listTextureFiles();
  list.removeDuplicates();
  return list;
}

// update the list of robots and top level solids
void WbWorld::updateTopLevelLists() {
  mTopSolids.clear();
  mTopSolids = WbNodeUtilities::findSolidDescendants(mRoot);
}

void WbWorld::removeRobotIfPresent(WbRobot *robot) {
  if (!robot)
    return;

  mRobots.removeAll(robot);
}

void WbWorld::addRobotIfNotAlreadyPresent(WbRobot *robot) {
  if (!robot)
    return;

  // don't add a robot that's already in the global list
  if (mRobots.contains(robot))
    return;

  mRobots.append(robot);
  setUpControllerForNewRobot(robot);
  emit robotAdded(robot);
}

void WbWorld::updateProjectPath(const QString &oldPath, const QString &newPath) {
  const QFileInfo infoPath(mFileName);
  const QFileInfo infoNewPath(newPath);
  const QString newFilename = infoNewPath.absolutePath() + "/worlds/" + infoPath.fileName();
  if (QFile::exists(newFilename))
    mFileName = newFilename;
}

void WbWorld::setViewpoint(WbViewpoint *viewpoint) {
  bool viewpointHasChanged = mViewpoint != viewpoint;
  mViewpoint = viewpoint;
  if (viewpointHasChanged)
    emit viewpointChanged();
}

double WbWorld::orthographicViewHeight() const {
  return mViewpoint->orthographicViewHeight();
}

void WbWorld::setOrthographicViewHeight(double ovh) const {
  mViewpoint->setOrthographicViewHeight(ovh);
}

bool WbWorld::reloadPerspective() {
  delete mPerspective;
  mPerspective = new WbPerspective(mFileName);
  return mPerspective->load();
}

void WbWorld::appendOdeContact(const WbOdeContact &odeContact) {
  mOdeContactsMutex.lock();  // TODO: understand why this mutex is here. Related with MT-safe physics plugin?
  mOdeContacts << odeContact;
  mOdeContactsMutex.unlock();
}

void WbWorld::appendOdeImmersionGeom(const dImmersionGeom &immersionGeom) {
  mOdeContactsMutex.lock();  // TODO: check if this mutex is necessary and if it needs to be renamed
  mImmersionGeoms.append(immersionGeom);
  mOdeContactsMutex.unlock();
}

void WbWorld::awake() {
  double currentSimulationTime = WbSimulationState::instance()->time();
  if (currentSimulationTime > mLastAwakeningTime) {  // we don't want to awake all the world several times in the same step
    mLastAwakeningTime = currentSimulationTime;
    WbMFNode::Iterator it(mRoot->children());
    while (it.hasNext()) {
      WbGroup *const group = dynamic_cast<WbGroup *>(it.next());
      if (group)
        WbSolid::awakeSolids(group);
    }
  }
}

void WbWorld::retrieveNodeNamesWithOptionalRendering(QStringList &centerOfMassNodeNames, QStringList &centerOfBuoyancyNodeNames,
                                                     QStringList &supportPolygonNodeNames) const {
  centerOfMassNodeNames.clear();
  centerOfBuoyancyNodeNames.clear();
  supportPolygonNodeNames.clear();

  WbSolid *solid = NULL;
  const QList<WbNode *> &allNodes = mRoot->subNodes(true);
  for (int i = 0; i < allNodes.size(); ++i) {
    solid = dynamic_cast<WbSolid *>(allNodes[i]);
    if (solid && (solid->globalCenterOfMassRepresentationEnabled() || solid->centerOfBuoyancyRepresentationEnabled() ||
                  solid->supportPolygonRepresentationEnabled())) {
      const QString name = solid->computeUniqueName();
      if (solid->globalCenterOfMassRepresentationEnabled())
        centerOfMassNodeNames << name;
      if (solid->centerOfBuoyancyRepresentationEnabled())
        centerOfBuoyancyNodeNames << name;
      if (solid->supportPolygonRepresentationEnabled())
        supportPolygonNodeNames << name;
    }
  }
}

QString WbWorld::logWorldMetrics() const {
  int solidCount = 0;
  int jointCount = 0;
  int geomCount = 0;
  const QList<WbNode *> &allNodes = mRoot->subNodes(true);
  foreach (WbNode *node, allNodes) {
    if (dynamic_cast<WbBasicJoint *>(node)) {
      jointCount++;
      continue;
    }
    WbSolid *solid = dynamic_cast<WbSolid *>(node);
    if (solid && (solid->isKinematic() || solid->isSolidMerger())) {
      solidCount++;
      continue;
    }
    WbGeometry *geometry = dynamic_cast<WbGeometry *>(node);
    if (geometry && !geometry->isInBoundingObject())
      geomCount++;
  }

  return QString("%1 solids, %2 joints, %3 graphical geometries").arg(solidCount).arg(jointCount).arg(geomCount);
}
