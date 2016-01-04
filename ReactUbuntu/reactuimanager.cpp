
#include <iterator>
#include <algorithm>

#include <QMetaObject>
#include <QMetaMethod>
#include <QJsonDocument>
#include <QQuickItem>

#include <QDebug>

#include "reactbridge.h"
#include "reactcomponentdata.h"
#include "reactmoduledata.h"
#include "reactflexlayout.h"
#include "reactattachedproperties.h"
#include "reactviewmanager.h"
#include "reactuimanager.h"


int ReactUIManager::m_nextRootTag = 1;


void ReactUIManager::removeSubviewsFromContainerWithID(int containerReactTag)
{
  qDebug() << __PRETTY_FUNCTION__;

  QQuickItem* item = m_views.value(containerReactTag);
  if (item == nullptr) {
    qWarning() << __PRETTY_FUNCTION__ << "Attempting to access unknown view";
    return;
  }

  QList<int> childIndexes;
  childIndexes.reserve(item->childItems().size());
  std::iota(childIndexes.begin(), childIndexes.end(), 0);

  manageChildren(containerReactTag, QList<int>(), QList<int>(), QList<int>(), QList<int>(), childIndexes);
}

void ReactUIManager::measure
(
 int reactTag,
 const ReactModuleInterface::ResponseBlock& callback
)
{
  qDebug() << __PRETTY_FUNCTION__;

  QQuickItem* item = m_views.value(reactTag);
  if (item == nullptr) {
    qWarning() << "Attempting to access unknown view";
    callback(m_bridge, QVariantList{});
    return;
  }

  QPointF rvo(item->x(), item->y());
  rvo = item->mapToItem(m_bridge->visualParent(), rvo);

  callback(m_bridge, QVariantList{
      item->x(),
      item->y(),
      item->width(),
      item->height(),
      rvo.x(),
      rvo.y()
    });
}

void ReactUIManager::updateView
(
  int reactTag,
  const QString& viewName,
  const QVariantMap& properties
)
{
  qDebug() << __PRETTY_FUNCTION__ << reactTag << viewName << properties;

  QQuickItem* item = m_views.value(reactTag);
  if (item == nullptr) {
    qWarning() << "Attempting to update properties on unknown view";
    return;
  }

  ReactAttachedProperties::get(item)->viewManager()->applyProperties(item, properties);

  m_bridge->visualParent()->polish();
}

void ReactUIManager::manageChildren
(
  int containerReactTag,
  const QList<int>& moveFromIndicies,
  const QList<int>& moveToIndices,
  const QList<int>& addChildReactTags,
  const QList<int>& addAtIndices,
  const QList<int>& removeAtIndices
)
{
  qDebug() << __PRETTY_FUNCTION__ << containerReactTag << moveFromIndicies << moveToIndices << addChildReactTags << addAtIndices << removeAtIndices;

  QQuickItem* container = m_views[containerReactTag];
  if (container == nullptr) {
    qWarning() << "Attempting to manage children on an unknown container";
    return;
  }

  QList<QQuickItem*> children;

  if (!removeAtIndices.isEmpty()) {
    // removeAtIndices get unpluged and erased
    children = container->childItems();
    std::sort(children.begin(), children.end(), [](QQuickItem* a, QQuickItem* b) {
          return a->z() < b->z();
        });
    for (int i : removeAtIndices) {
      QQuickItem* item = children[i];
      ReactAttachedProperties* rap = ReactAttachedProperties::get(item);
      if (rap == nullptr) {
        qCritical() << "Attempting to manage non react view!";
        return;
      }
      m_views.remove(rap->tag());
      item->setParent(0);
      item->deleteLater();
    }
  }

  children.clear();
  // XXX: Assumption - addChildReactTags is sorted
  std::transform(addChildReactTags.begin(), addChildReactTags.end(),
                 std::back_inserter(children),
                 [this](int key) {
                   return m_views.value(key);
                 });

  // on iOS, order of the subviews implies z-order, implicitly its the same in
  // QML, barring some exceptions. revisit - set zorder appears to be the only
  // exception can probably self order items, but it's not an explicit guarantee
  QList<QQuickItem*>::iterator it = children.begin();
  for (int i : addAtIndices) {
    QQuickItem* child = *it++;
    child->setParentItem(container);
    child->setZ(i);
    ReactFlexLayout::get(child)->setDirty(true);
  }

  m_bridge->visualParent()->polish();
}

// Reacts version of first responder
void ReactUIManager::setJSResponder(int reactTag, bool blockNativeResponder)
{
  Q_UNUSED(reactTag);
  Q_UNUSED(blockNativeResponder);

  qDebug() << __PRETTY_FUNCTION__;
}

void ReactUIManager::clearJSResponder()
{
}

// in iOS, resign first responder (actual)
void ReactUIManager::blur(int reactTag)
{
  Q_UNUSED(reactTag);

  qDebug() << __PRETTY_FUNCTION__;
}

void ReactUIManager::createView
(
  int reactTag,
  const QString& viewName,
  int rootTag,
  const QVariantMap& props
)
{
  Q_UNUSED(rootTag);

  qDebug() << __PRETTY_FUNCTION__ << reactTag << viewName << rootTag << props;
  ReactComponentData* cd = m_componentData.value(viewName);
  if (cd == nullptr) {
    qCritical() << "Attempt to create unknown view of type" << viewName;
    return;
  }

  QQuickItem* item = cd->createView(reactTag, props);
  if (item == nullptr) {
    qWarning() << "Failed to create view of type" << viewName;
    return;
  }

  ReactAttachedProperties* rap = ReactAttachedProperties::get(item);
  rap->setTag(reactTag);
  rap->setViewManager(cd->manager());

  // At creation properties have been applied which can lead to the new item's
  // layout being marked as dirty - but we want to be able to mark items being
  // positioned in the visual hierarchy as dirty, so force a reset until that
  // time.
  ReactFlexLayout::get(item)->setDirty(false);

  m_views.insert(reactTag, item);
}

void ReactUIManager::findSubviewIn
(
 int reactTag,
 const QPointF& point,
 const ReactModuleInterface::ResponseBlock& callback
)
{
  QQuickItem* item = m_views.value(reactTag);
  if (item == nullptr) {
    qWarning() << "Attempting to access unknown view";
    callback(m_bridge, QVariantList{});
    return;
  }

  // Find the deepest match
  QQuickItem* target = nullptr;
  QQuickItem* next = item;
  QPointF local = point;
  forever {
    target = next;
    next = target->childAt(local.x(), local.y());
    if (next == nullptr || !next->isEnabled())
      break;
    local = target->mapToItem(next, local);
  }

  // XXX: should climb back up to a matching react target?
  ReactAttachedProperties* properties = ReactAttachedProperties::get(target, false);
  if (properties == nullptr) {
    qWarning() << "Found target on a non react view";
    callback(m_bridge, QVariantList{});
    return;
  }

  QRectF frame = item->mapRectFromItem(target,
                                       QRectF(target->x(), target->y(),
                                              target->width(), target->height()));
  callback(m_bridge, QVariantList{
      properties->tag(),
      frame.x(),
      frame.y(),
      frame.width(),
      frame.height()
    });
}


ReactUIManager::ReactUIManager()
  : m_bridge(nullptr)
{
}

ReactUIManager::~ReactUIManager()
{
}

void ReactUIManager::setBridge(ReactBridge* bridge)
{
  qDebug() << __PRETTY_FUNCTION__;
  if (m_bridge != nullptr) {
    qCritical() << "Bridge already set, UIManager already initialised?";
    return;
  }

  m_bridge = bridge;

  for (ReactModuleData* data : m_bridge->modules()) {
    ReactViewManager* manager = data->viewManager();
    if (manager != nullptr) {
      ReactComponentData* cd = new ReactComponentData(manager);
      m_componentData.insert(cd->name(), cd);
    }
  }

  connect(m_bridge->visualParent(), SIGNAL(widthChanged()), SLOT(rootViewWidthChanged()));
  connect(m_bridge->visualParent(), SIGNAL(heightChanged()), SLOT(rootViewHeightChanged()));
  connect(m_bridge->visualParent(), SIGNAL(scaleChanged()), SLOT(rootViewScaleChanged()));
}

QString ReactUIManager::moduleName()
{
  return "RCTUIManager";
}

QStringList ReactUIManager::methodsToExport()
{
  const QMetaObject* metaObject = this->metaObject();
  const int methodCount = metaObject->methodCount();

  QStringList methods;
  for (int i = metaObject->methodOffset(); i < methodCount; ++i) {
    methods << metaObject->method(i).name();
  }
  return methods;
}

QVariantMap ReactUIManager::constantsToExport()
{
  QVariantMap rc;
  QVariantMap directEvents;
  QVariantMap bubblingEvents;

  for (const ReactComponentData* componentData : m_componentData) {
    qDebug() << "Checking" << componentData->name();

    QVariantMap managerInfo;

    QVariantMap config = componentData->viewConfig();
    if (!config.isEmpty()) {
      managerInfo.insert("NativeProps", config["propTypes"]);
    }

    for (const QString& eventName : config["directEvents"].toStringList()) {
      if (!directEvents.contains(eventName)) {
        QString tmp = eventName; tmp.replace(0, 3, "on");
        directEvents.insert(eventName,
                            QVariantMap{{"registrationName", tmp}});
      }
    }

    for (const QString& eventName : config["bubblingEvents"].toStringList()) {
      if (!bubblingEvents.contains(eventName)) {
        QString tmp = eventName; tmp.replace(0, 3, "on");
        bubblingEvents.insert(eventName,
                              QVariantMap{{"phasedRegistrationNames",
                                  QVariantMap{{"bubbled", tmp},
                                              {"captured", tmp.append("Capture")}}}});
      }
    }

    rc.insert(componentData->name(), managerInfo);
  }

  rc.insert("customBubblingEventTypes", bubblingEvents);
  rc.insert("customDirectEventTypes", directEvents);
  rc.insert("Dimensions",
            QVariantMap{
              { "width", m_bridge->visualParent()->width() },
              { "height", m_bridge->visualParent()->height() },
              { "scale", m_bridge->visualParent()->scale() }});
  rc.insert("modalFullscreenView",
            QVariantMap{
              { "width", m_bridge->visualParent()->width() },
              { "height", m_bridge->visualParent()->height() }});

  return rc;
}

int ReactUIManager::allocateRootTag()
{
  int tag = m_nextRootTag;
  m_nextRootTag += 10;
  return tag;
}

void ReactUIManager::registerRootView(QQuickItem* root)
{
  ReactAttachedProperties* properties = ReactAttachedProperties::get(root);
  m_views.insert(properties->tag(), root);
}

void ReactUIManager::rootViewWidthChanged()
{
  QQuickItem* root = m_bridge->visualParent();
  if (ReactAttachedProperties::get(root)->tag() == -1)
    return;
  root->polish();
}

void ReactUIManager::rootViewHeightChanged()
{
  QQuickItem* root = m_bridge->visualParent();
  if (ReactAttachedProperties::get(root)->tag() == -1)
    return;
  root->polish();
}

void ReactUIManager::rootViewScaleChanged()
{
  QQuickItem* root = m_bridge->visualParent();
  if (ReactAttachedProperties::get(root)->tag() == -1)
    return;
  root->polish();
}
