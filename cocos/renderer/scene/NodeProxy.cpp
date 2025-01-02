/****************************************************************************
 Copyright (c) 2018 Xiamen Yaji Software Co., Ltd.

 http://www.cocos2d-x.org

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "NodeProxy.hpp"

#include <string>

#include "ModelBatcher.hpp"
#include "../renderer/Scene.h"
#include "base/ccMacros.h"
#include "cocos/scripting/js-bindings/jswrapper/SeApi.h"
#include "cocos/scripting/js-bindings/manual/jsb_conversions.hpp"
#include "cocos/scripting/js-bindings/auto/jsb_renderer_auto.hpp"
#include "NodeMemPool.hpp"
#include <math.h>
#include "RenderFlow.hpp"
#include "assembler/AssemblerSprite.hpp"

RENDERER_BEGIN

uint32_t NodeProxy::_globalRenderOrder = 0;

NodeProxy::NodeProxy(std::size_t unitID, std::size_t index, const std::string &id, const std::string &name)
{
    traverseHandle = render;

    _id = id;
    _unitID = unitID;
    _index = index;
    _name = name;

    NodeMemPool *pool = NodeMemPool::getInstance();
    CCASSERT(pool, "NodeProxy constructor NodeMemPool is null");
    UnitNode *unit = pool->getUnit(unitID);
    CCASSERT(unit, "NodeProxy constructor unit is null");

    UnitCommon *common = pool->getCommonUnit(unitID);
    _signData = common->getSignData(_index);

    _dirty = unit->getDirty(index);
    *_dirty &= ~RenderFlow::PRE_CALCULATE_VERTICES;

    _trs = unit->getTRS(index);
    _localMat = unit->getLocalMat(index);
    _worldMat = unit->getWorldMat(index);
    _parentInfo = unit->getParent(index);
    _parentInfo->unitID = PARENT_INVALID;
    _parentInfo->index = PARENT_INVALID;
    _localZOrder = unit->getZOrder(index);
    _cullingMask = unit->getCullingMask(index);
    _opacity = unit->getOpacity(index);
    _is3DNode = unit->getIs3D(index);
    _skew = unit->getSkew(index);

    uint64_t *self = unit->getNode(index);
    *self = (uint64_t)this;
}

NodeProxy::~NodeProxy()
{
    for (auto &child : _children)
    {
        child->_parent = nullptr;
    }

    CC_SAFE_RELEASE(_assembler);
}

void NodeProxy::destroyImmediately()
{
    if (_parent)
    {
        _parent->removeChild(this);
    }
    RenderFlow::getInstance()->removeNodeLevel(_level, _worldMat);
    CC_SAFE_RELEASE_NULL(_assembler);
    _level = NODE_LEVEL_INVALID;
    _dirty = nullptr;
    _trs = nullptr;
    _localMat = nullptr;
    _worldMat = nullptr;
    _parentInfo = nullptr;
    _localZOrder = nullptr;
    _cullingMask = nullptr;
    _opacity = nullptr;
    _is3DNode = nullptr;
    _skew = nullptr;
    // 动态创建的List 内存清理
    if (_recordCleanVectorFlag)
    {
        _bfsIndexList->clear();
        delete _bfsIndexList;
        _bfsMaskList->clear();
        delete _bfsMaskList;
        for (auto iter = _bfsMap->begin(); iter != _bfsMap->end(); ++iter)
        {
            auto valueList = iter->second;
            valueList.clear();
        }
        _bfsMap->clear();
        delete _bfsMap;
    }
    _bfsIndexList = nullptr;
    _bfsMap = nullptr;
    _bfsMaskList = nullptr;
}

// lazy allocs
void NodeProxy::childrenAlloc()
{
    _children.reserve(4);
}

void NodeProxy::addChild(NodeProxy *child)
{
    if (child == nullptr)
    {
        CCLOGWARN("Argument must be non-nil");
        return;
    }
    if (child->_parent != nullptr)
    {
        CCLOGWARN("child already added. It can't be added again");
        return;
    }
    auto assertNotSelfChild([this, child]() -> bool
                            {
              for ( NodeProxy* parent( this ); parent != nullptr;
                    parent = parent->getParent() )
                  if ( parent == child )
                      return false;
              
              return true; });
    (void)assertNotSelfChild;

    if (!assertNotSelfChild())
    {
        CCLOGWARN("A node cannot be the child of his own children");
        return;
    }

    if (_children.empty())
    {
        this->childrenAlloc();
    }
    _children.pushBack(child);
    child->setParent(this);
}
void NodeProxy::cleanBfsChildren(NodeProxy *child)
{
    child->_bfsRenderFlag = false;
    if (child->_bfsIndexList != nullptr)
    {
        if (child->_bfsNodeKey != "")
        {
            // bfs 从 _bfsMap 中移除渲染无效的节点
            std::vector<NodeProxy *> &list = (*child->_bfsMap)[child->_bfsNodeKey];
            for (int32_t i = 0, l = list.size(); i < l; i++)
            {
                NodeProxy *childOne = list[i];
                if (childOne->getID() == child->getID())
                {
                    child->_bfsIndexList = nullptr;
                    list.erase(list.begin() + i);
                    break;
                }
            }
        }
        // 递归清理子节点标识
        cocos2d::Vector<NodeProxy *> chidren = child->getChildren();
        if (chidren.empty())
        {
            return;
        }
        for (int32_t c = 0; c < chidren.size(); c++)
        {
            cleanBfsChildren(*(chidren.begin() + c));
        }
    }
}

void NodeProxy::detachChild(NodeProxy *child, ssize_t childIndex)
{
    // set parent nil at the end
    child->setParent(nullptr);
    cleanBfsChildren(child);
    _children.erase(childIndex);
}

void NodeProxy::removeChild(NodeProxy *child)
{
    // explicit nil handling
    if (_children.empty())
    {
        return;
    }
    ssize_t index = _children.getIndex(child);
    if (index != CC_INVALID_INDEX)
        this->detachChild(child, index);
}

void NodeProxy::removeAllChildren()
{
    // not using detachChild improves speed here
    for (const auto &child : _children)
    {
        // set parent nil at the end
        child->setParent(nullptr);
    }

    _children.clear();
}

NodeProxy *NodeProxy::getChildByName(std::string childName)
{
    for (auto child : _children)
    {
        if (child->_name == childName)
        {
            return child;
        }
    }
    return nullptr;
}

NodeProxy *NodeProxy::getChildByID(std::string id)
{
    for (auto child : _children)
    {
        if (child->_id == id)
        {
            return child;
        }
    }
    return nullptr;
}

void NodeProxy::notifyUpdateParent()
{
    if (_parentInfo->index == PARENT_INVALID)
    {
        if (_parent)
        {
            _parent->removeChild(this);
        }
        updateLevel();
        return;
    }

    NodeMemPool *pool = NodeMemPool::getInstance();
    CCASSERT(pool, "NodeProxy updateParent NodeMemPool is null");
    UnitNode *unit = pool->getUnit(_parentInfo->unitID);
    CCASSERT(unit, "NodeProxy updateParent unit is null");
    uint64_t *parentAddrs = unit->getNode(_parentInfo->index);
    NodeProxy *parent = (NodeProxy *)*parentAddrs;
    CCASSERT(parent, "NodeProxy updateParent parent is null");

    if (parent != _parent)
    {
        if (_parent)
        {
            _parent->removeChild(this);
        }
        parent->addChild(this);
        updateLevel();
        // bfs  标识状态添加，所有子节点使用bfs 接管渲染
        bool needBfsRender = *_dirty & RenderFlow::CHILDREN_BFS_RENDER;
        if (needBfsRender)
        {
            if (!_bfsIndexList)
            {
                _bfsIndexList = new std::vector<std::string>;
                _bfsMap = new std::unordered_map<std::string, std::vector<NodeProxy *>>;
                _bfsMaskList = new std::vector<NodeProxy *>;
                _bfsRenderFlag = true;
                _recordCleanVectorFlag = true;
                markBfsRenderFlag(this);
            }
        }
        if (_parent->_bfsRenderFlag)
        {
            this->_markBfsRenderFalg(this);
            markBfsRenderFlag(this);
        }
    }
}

void NodeProxy::_markBfsRenderFalg(NodeProxy *it)
{
    // mask 单独抽离放入一个vector 中，然后对其子节点使用bfs 渲染模式。
    if ((it)->_assembler && dynamic_cast<MaskAssembler *>(it->_assembler))
    {
        (it)->_bfsIndexList = new std::vector<std::string>;
        (it)->_bfsMap = new std::unordered_map<std::string, std::vector<NodeProxy *>>;
        (it)->_bfsMaskList = new std::vector<NodeProxy *>;
        (it)->_recordCleanVectorFlag = true;
        (it)->_parent->_bfsMaskList->push_back(it);
    }
    else
    {
        if (!(it)->_parent->_bfsIndexList)
        {
            (it)->_parent->_bfsIndexList = new std::vector<std::string>;
            (it)->_parent->_bfsMap = new std::unordered_map<std::string, std::vector<NodeProxy *>>;
            (it)->_parent->_bfsMaskList = new std::vector<NodeProxy *>;
            (it)->_parent->_recordCleanVectorFlag = true;
        }
        (it)->_bfsIndexList = (it)->_parent->_bfsIndexList;
        (it)->_bfsMaskList = (it)->_parent->_bfsMaskList;
        (it)->_bfsMap = (it)->_parent->_bfsMap;
        std::string key = (it)->_parent->getName() + "_" + (it)->getName();
        it->_bfsNodeKey = key;
        if ((it)->_parent->_bfsMap->find(key) == (it)->_parent->_bfsMap->end())
        {
            (it)->_parent->_bfsIndexList->push_back(key);
        }
        (*(it)->_parent->_bfsMap)[key].push_back((it));
    }
    (it)->_bfsRenderFlag = true;
}

void NodeProxy::markBfsRenderFlag(NodeProxy *node)
{
    for (auto it = node->_children.begin(); it != node->_children.end(); it++)
    {
        if (!(*it)->_bfsRenderFlag)
        {
            (*it)->_markBfsRenderFalg(*it);
            markBfsRenderFlag(*it);
        }
    }
}

void NodeProxy::updateLevel()
{
    static RenderFlow::LevelInfo levelInfo;
    auto renderFlow = RenderFlow::getInstance();

    renderFlow->removeNodeLevel(_level, _worldMat);

    levelInfo.dirty = _dirty;
    levelInfo.localMat = _localMat;
    levelInfo.worldMat = _worldMat;
    levelInfo.opacity = _opacity;
    levelInfo.realOpacity = &_realOpacity;

    if (_parent)
    {
        _level = _parent->_level + 1;
        levelInfo.parentWorldMat = _parent->_worldMat;
        levelInfo.parentDirty = _parent->_dirty;
        levelInfo.parentRealOpacity = &_parent->_realOpacity;
    }
    else
    {
        _level = 0;
        levelInfo.parentWorldMat = nullptr;
        levelInfo.parentDirty = nullptr;
        levelInfo.parentRealOpacity = nullptr;
    }
    renderFlow->insertNodeLevel(_level, levelInfo);

    for (auto it = _children.begin(); it != _children.end(); it++)
    {
        (*it)->updateLevel();
    }
}

void NodeProxy::setLocalZOrder(int zOrder)
{
    if (*_localZOrder != zOrder)
    {
        *_localZOrder = zOrder;
        if (_parent != nullptr)
        {
            *_parent->_dirty |= RenderFlow::REORDER_CHILDREN;
        }
    }
}

void NodeProxy::reorderChildren()
{
    if (*_dirty & RenderFlow::REORDER_CHILDREN)
    {
#if CC_64BITS
        std::sort(std::begin(_children), std::end(_children), [](NodeProxy *n1, NodeProxy *n2)
                  { return (*n1->_localZOrder < *n2->_localZOrder); });
#else
        std::stable_sort(std::begin(_children), std::end(_children), [](NodeProxy *n1, NodeProxy *n2)
                         { return *n1->_localZOrder < *n2->_localZOrder; });
#endif
        *_dirty &= ~RenderFlow::REORDER_CHILDREN;
    }
}

void NodeProxy::setAssembler(AssemblerBase *assembler)
{
    if (assembler == _assembler)
        return;
    CC_SAFE_RELEASE(_assembler);
    _assembler = assembler;
    CC_SAFE_RETAIN(_assembler);

    auto assemblerSprite = dynamic_cast<AssemblerSprite *>(_assembler);
    if (assemblerSprite)
    {
        *_dirty |= RenderFlow::PRE_CALCULATE_VERTICES;
    }
    else
    {
        *_dirty &= ~RenderFlow::PRE_CALCULATE_VERTICES;
    }
}

void NodeProxy::clearAssembler()
{
    CC_SAFE_RELEASE_NULL(_assembler);
    *_dirty &= ~RenderFlow::PRE_CALCULATE_VERTICES;
}

AssemblerBase *NodeProxy::getAssembler() const
{
    return _assembler;
}

void NodeProxy::getPosition(cocos2d::Vec3 *out) const
{
    out->x = _trs->x;
    out->y = _trs->y;
    out->z = _trs->z;
}

void NodeProxy::getRotation(cocos2d::Quaternion *out) const
{
    out->x = _trs->qx;
    out->y = _trs->qy;
    out->z = _trs->qz;
    out->w = _trs->qw;
}

void NodeProxy::getScale(cocos2d::Vec3 *out) const
{
    out->x = _trs->sx;
    out->y = _trs->sy;
    out->z = _trs->sz;
}

void NodeProxy::getWorldRotation(cocos2d::Quaternion *out) const
{
    getRotation(out);

    cocos2d::Quaternion rot;
    NodeProxy *curr = _parent;
    while (curr != nullptr)
    {
        curr->getRotation(&rot);
        Quaternion::multiply(rot, *out, out);
        curr = curr->getParent();
    }
}

void NodeProxy::getWorldPosition(cocos2d::Vec3 *out) const
{
    getPosition(out);

    cocos2d::Vec3 pos;
    cocos2d::Quaternion rot;
    cocos2d::Vec3 scale;
    NodeProxy *curr = _parent;
    while (curr != nullptr)
    {
        curr->getPosition(&pos);
        curr->getRotation(&rot);
        curr->getScale(&scale);

        out->multiply(scale);
        out->transformQuat(rot);
        out->add(pos);
        curr = curr->getParent();
    }
}

void NodeProxy::getWorldRT(cocos2d::Mat4 *out) const
{
    cocos2d::Vec3 opos(_trs->x, _trs->y, _trs->z);
    cocos2d::Quaternion orot(_trs->qx, _trs->qy, _trs->qz, _trs->qw);

    cocos2d::Vec3 pos;
    cocos2d::Quaternion rot;
    cocos2d::Vec3 scale;
    NodeProxy *curr = _parent;
    while (curr != nullptr)
    {
        curr->getPosition(&pos);
        curr->getRotation(&rot);
        curr->getScale(&scale);

        opos.multiply(scale);
        opos.transformQuat(rot);
        opos.add(pos);
        Quaternion::multiply(rot, orot, &orot);
        curr = curr->getParent();
    }
    out->setIdentity();
    out->translate(opos);
    cocos2d::Mat4 quatMat;
    cocos2d::Mat4::createRotation(orot, &quatMat);
    out->multiply(quatMat);
}

void NodeProxy::setOpacity(uint8_t opacity)
{
    if (*_opacity != opacity)
    {
        *_opacity = opacity;
        *_dirty |= RenderFlow::OPACITY;
    }
}

void NodeProxy::updateRealOpacity()
{
    bool selfOpacityDirty = *_dirty & RenderFlow::OPACITY;
    if (_parent)
    {
        if (selfOpacityDirty || *_parent->_dirty & RenderFlow::NODE_OPACITY_CHANGED)
        {
            _realOpacity = *_opacity * _parent->getRealOpacity() / 255.0f;
            *_dirty &= ~RenderFlow::OPACITY;
            *_dirty |= RenderFlow::NODE_OPACITY_CHANGED;
        }
    }
    else
    {
        if (selfOpacityDirty)
        {
            _realOpacity = *_opacity;
            *_dirty &= ~RenderFlow::OPACITY;
            *_dirty |= RenderFlow::NODE_OPACITY_CHANGED;
        }
    }
}

void NodeProxy::updateWorldMatrix()
{
    if (!_updateWorldMatrix)
        return;

    bool selfWorldDirty = *_dirty & RenderFlow::WORLD_TRANSFORM;
    if (_parent)
    {
        if (selfWorldDirty || *_parent->_dirty & RenderFlow::WORLD_TRANSFORM_CHANGED)
        {
            cocos2d::Mat4::multiply(_parent->getWorldMatrix(), *_localMat, _worldMat);
            *_dirty &= ~RenderFlow::WORLD_TRANSFORM;
            *_dirty |= RenderFlow::WORLD_TRANSFORM_CHANGED;
        }
    }
    else if (selfWorldDirty)
    {
        *_worldMat = *_localMat;
        *_dirty &= ~RenderFlow::WORLD_TRANSFORM;
        *_dirty |= RenderFlow::WORLD_TRANSFORM_CHANGED;
    }
}

void NodeProxy::updateWorldMatrix(const cocos2d::Mat4 &worldMatrix)
{
    *_worldMat = worldMatrix;
    *_dirty &= ~RenderFlow::WORLD_TRANSFORM;
    *_dirty |= RenderFlow::WORLD_TRANSFORM_CHANGED;
}

void NodeProxy::updateLocalMatrix()
{
    bool skew = std::abs(_skew->x - 0.0f) > MATH_EPSILON || std::abs(_skew->y - 0.0f) > MATH_EPSILON;
    if (*_dirty & RenderFlow::LOCAL_TRANSFORM || skew)
    {
        _localMat->setIdentity();

        // Transform = Translate * Rotation * Scale;
        cocos2d::Quaternion q(_trs->qx, _trs->qy, _trs->qz, _trs->qw);
        if (*_is3DNode)
        {
            _localMat->translate(_trs->x, _trs->y, _trs->z);
            _localMat->rotate(q);
            _localMat->scale(_trs->sx, _trs->sy, _trs->sz);
        }
        else
        {
            _localMat->translate(_trs->x, _trs->y, 0);
            _localMat->rotate(q);
            _localMat->scale(_trs->sx, _trs->sy, 1);
        }

        if (skew)
        {
            auto &m = _localMat->m;
            auto a = m[0];
            auto b = m[1];
            auto c = m[4];
            auto d = m[5];
            auto skx = (float)tanf(CC_DEGREES_TO_RADIANS(_skew->x));
            auto sky = (float)tanf(CC_DEGREES_TO_RADIANS(_skew->y));
            m[0] = a + c * sky;
            m[1] = b + d * sky;
            m[4] = c + a * skx;
            m[5] = d + b * skx;
        }

        *_dirty &= ~RenderFlow::LOCAL_TRANSFORM;
        *_dirty |= RenderFlow::WORLD_TRANSFORM;
    }
}

/**
 *  自定义 bfs 渲染
 */
void NodeProxy::childrenBfsRender(NodeProxy *node, ModelBatcher *batcher, Scene *scene)
{
    if (node->_bfsIndexList->size() > 0)
    {
        for (auto key : *(node->_bfsIndexList))
        {
            std::vector<NodeProxy *> &list = (*node->_bfsMap)[key];
            for (int32_t i = 0, l = list.size(); i < l; i++)
            {
                NodeProxy *child = list[i];
                // CCLOG("ChildRenBfsRender %d%d",i,l);
                if (!child || !child->isValid() || !child->_parent)
                {
                    if (child->_bfsIndexList != nullptr)
                    {
                        child->removeAllChildren();
                    }
                    list.erase(list.begin() + i);
                    i--;
                    l = list.size();
                    continue;
                }
                // 透明度或者needVisit 问题
                if (child->_parent->_recordOpacityOrActive)
                {
                    child->_recordOpacityOrActive = child->_parent->_recordOpacityOrActive;
                    continue;
                }
                if (!child->_needVisit || child->_realOpacity == 0)
                {
                    child->_recordOpacityOrActive = true;
                    continue;
                }
                else
                {
                    child->_recordOpacityOrActive = false;
                }
                // 渲染节点
                renderNode(child, batcher, scene);
                // post render
                bool needPostRender = *(child->_dirty) & RenderFlow::POST_RENDER;
                if (child->_assembler && needPostRender)
                    child->_assembler->postHandle(child, batcher, scene);
            }
        }
        // 对子节点包含scrollview  mask 组件的节点单独处理渲染
        if (node->_bfsMaskList && node->_bfsMaskList->size() > 0)
        {
            for (std::size_t i = 0, l = node->_bfsMaskList->size(); i < l; i++)
            {
                NodeProxy *child = node->_bfsMaskList->at(i);
                if (!child->isValid())
                {
                    node->_bfsMaskList->erase(node->_bfsMaskList->begin() + i);
                    i--;
                    l = node->_bfsMaskList->size();
                    continue;
                }
                // 透明度或者needVisit 问题
                if (child->_recordOpacityOrActive)
                    continue;
                // mask渲染
                renderNode(child, batcher, scene);
                childrenBfsRender(child, batcher, scene);
                // post render
                bool needPostRender = *(child->_dirty) & RenderFlow::POST_RENDER;
                if (child->_assembler && needPostRender)
                    child->_assembler->postHandle(child, batcher, scene);
            }
        }
    }
}
/**
 * 遍历子节点
 */
void NodeProxy::traverseChildren(NodeProxy *node, ModelBatcher *batcher, Scene *scene)
{
    for (const auto &child : node->_children)
    {
        auto traverseHandle = child->traverseHandle;
        traverseHandle(child, batcher, scene);
    }
}

void NodeProxy::renderNode(NodeProxy *node, ModelBatcher *batcher, Scene *scene)
{
    bool needRender = false;
    needRender = *node->_dirty & RenderFlow::RENDER;
    if (node->_needRender != needRender)
    {
        if (node->_assembler)
            node->_assembler->enableDirty(AssemblerBase::VERTICES_OPACITY_CHANGED);
        node->_needRender = needRender;
    }
    // pre render
    if (node->_assembler && needRender)
        node->_assembler->handle(node, batcher, scene);
}

void NodeProxy::render(NodeProxy *node, ModelBatcher *batcher, Scene *scene)
{

    node->_renderOrder = _globalRenderOrder++;
    // bfs 标记
    bool needBfsRender = *node->_dirty & RenderFlow::CHILDREN_BFS_RENDER;
    if (needBfsRender)
    {
        if (!node->_bfsIndexList)
        {
            node->_bfsIndexList = new std::vector<std::string>;
            node->_bfsMap = new std::unordered_map<std::string, std::vector<NodeProxy *>>;
            node->_bfsMaskList = new std::vector<NodeProxy *>;
            node->_recordCleanVectorFlag = true;
            node->markBfsRenderFlag(node);
        }
    }
    if (!node->_needVisit || node->_realOpacity == 0)
    {
        return;
    }
    if (*node->_dirty & RenderFlow::DONOTHING)
    {
        return;
    }

    node->_recordOpacityOrActive = false;
    // 标记bfs 激活
    if (needBfsRender)
    {
        node->_bfsRenderFlag = true;
        renderNode(node, batcher, scene);
    }
    // bfs render 标记的后面渲染
    if (!node->_bfsRenderFlag)
    {
        renderNode(node, batcher, scene);
    }
    // 子节点排序
    if (!node->_recordOpacityOrActive)
        node->reorderChildren();

    // 非bfs 走正常的渲染流程。
    if (!needBfsRender)
    {
        // 递归子节点
        for (const auto &child : node->_children)
        {
            auto traverseHandle = child->traverseHandle;
            traverseHandle(child, batcher, scene);
        }
    }
    else
    {
        // bfs 渲染
        childrenBfsRender(node, batcher, scene);
    }

    // pos render 也需判定
    if (!node->_bfsRenderFlag)
    {
        // post render
        bool needPostRender = *(node->_dirty) & RenderFlow::POST_RENDER;
        if (node->_assembler && needPostRender)
            node->_assembler->postHandle(node, batcher, scene);
    }
}

void NodeProxy::visit(NodeProxy *node, ModelBatcher *batcher, Scene *scene)
{
    node->_renderOrder = _globalRenderOrder++;

    if (!node->_needVisit)
        return;

    node->updateRealOpacity();

    if (node->_realOpacity == 0)
    {
        return;
    }

    node->updateLocalMatrix();
    node->updateWorldMatrix();

    bool needRender = *(node->_dirty) & RenderFlow::RENDER;
    if (node->_needRender != needRender)
    {
        if (node->_assembler)
            node->_assembler->enableDirty(AssemblerBase::VERTICES_OPACITY_CHANGED);
        node->_needRender = needRender;
    }

    // pre render
    if (node->_assembler && needRender)
        node->_assembler->handle(node, batcher, scene);

    node->reorderChildren();
    for (const auto &child : node->_children)
    {
        visit(child, batcher, scene);
    }

    // post render
    bool needPostRender = *(node->_dirty) & RenderFlow::POST_RENDER;
    if (node->_assembler && needPostRender)
        node->_assembler->postHandle(node, batcher, scene);
}

RENDERER_END
