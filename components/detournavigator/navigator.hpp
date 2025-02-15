﻿#ifndef OPENMW_COMPONENTS_DETOURNAVIGATOR_NAVIGATOR_H
#define OPENMW_COMPONENTS_DETOURNAVIGATOR_NAVIGATOR_H

#include "objectid.hpp"
#include "sharednavmeshcacheitem.hpp"
#include "recastmeshtiles.hpp"
#include "waitconditiontype.hpp"
#include "heightfieldshape.hpp"
#include "objecttransform.hpp"

#include <components/resource/bulletshape.hpp>

#include <string_view>

namespace ESM
{
    struct Cell;
    struct Pathgrid;
}

namespace Loading
{
    class Listener;
}

namespace osg
{
    class Stats;
}

namespace DetourNavigator
{
    struct Settings;
    struct AgentBounds;

    struct ObjectShapes
    {
        osg::ref_ptr<const Resource::BulletShapeInstance> mShapeInstance;
        ObjectTransform mTransform;

        ObjectShapes(const osg::ref_ptr<const Resource::BulletShapeInstance>& shapeInstance, const ObjectTransform& transform)
            : mShapeInstance(shapeInstance)
            , mTransform(transform)
        {
            assert(mShapeInstance != nullptr);
        }
    };

    struct DoorShapes : ObjectShapes
    {
        osg::Vec3f mConnectionStart;
        osg::Vec3f mConnectionEnd;

        DoorShapes(const osg::ref_ptr<const Resource::BulletShapeInstance>& shapeInstance,
                   const ObjectTransform& transform, const osg::Vec3f& connectionStart, const osg::Vec3f& connectionEnd)
            : ObjectShapes(shapeInstance, transform)
            , mConnectionStart(connectionStart)
            , mConnectionEnd(connectionEnd)
        {}
    };

    /**
     * @brief Top level interface of detournavigator component. Navigator allows to build a scene with navmesh and find
     * a path for an agent there. Scene contains agents, geometry objects and water. Agent are distinguished only by
     * half extents. Each object has unique identifier and could be added, updated or removed. Water could be added once
     * for each world cell at given level of height. Navmesh builds asynchronously in separate threads. To start build
     * navmesh call update method.
     */
    struct Navigator
    {
        virtual ~Navigator() = default;

        /**
         * @brief addAgent should be called for each agent even if all of them has same half extents.
         * @param agentBounds allows to setup bounding cylinder for each agent, for each different half extents
         * there is different navmesh.
         */
        virtual void addAgent(const AgentBounds& agentBounds) = 0;

        /**
         * @brief removeAgent should be called for each agent even if all of them has same half extents
         * @param agentBounds allows determine which agent to remove
         */
        virtual void removeAgent(const AgentBounds& agentBounds) = 0;

        /**
         * @brief setWorldspace should be called before adding object from new worldspace
         * @param worldspace
         */
        virtual void setWorldspace(std::string_view worldspace) = 0;

        /**
         * @brief updateBounds should be called before adding object from loading cell
         * @param playerPosition corresponds to the bounds center
         */
        virtual void updateBounds(const osg::Vec3f& playerPosition) = 0;

        /**
         * @brief addObject is used to add complex object with allowed to walk and avoided to walk shapes
         * @param id is used to distinguish different objects
         * @param shape members must live until object is updated by another shape removed from Navigator
         * @param transform allows to setup objects geometry according to its world state
         * @return true if object is added, false if there is already object with given id
         */
        virtual bool addObject(const ObjectId id, const ObjectShapes& shapes, const btTransform& transform) = 0;

        /**
         * @brief addObject is used to add doors.
         * @param id is used to distinguish different objects.
         * @param shape members must live until object is updated by another shape or removed from Navigator.
         * @param transform allows to setup objects geometry according to its world state.
         * @return true if object is added, false if there is already object with given id.
         */
        virtual bool addObject(const ObjectId id, const DoorShapes& shapes, const btTransform& transform) = 0;

        /**
         * @brief updateObject replace object geometry by given data.
         * @param id is used to find object.
         * @param shape members must live until object is updated by another shape removed from Navigator.
         * @param transform allows to setup objects geometry according to its world state.
         * @return true if object is updated, false if there is no object with given id.
         */
        virtual bool updateObject(const ObjectId id, const ObjectShapes& shapes, const btTransform& transform) = 0;

        /**
         * @brief updateObject replace object geometry by given data.
         * @param id is used to find object.
         * @param shape members must live until object is updated by another shape removed from Navigator.
         * @param transform allows to setup objects geometry according to its world state.
         * @return true if object is updated, false if there is no object with given id.
         */
        virtual bool updateObject(const ObjectId id, const DoorShapes& shapes, const btTransform& transform) = 0;

        /**
         * @brief removeObject to make it no more available at the scene.
         * @param id is used to find object.
         * @return true if object is removed, false if there is no object with given id.
         */
        virtual bool removeObject(const ObjectId id) = 0;

        /**
         * @brief addWater is used to set water level at given world cell.
         * @param cellPosition allows to distinguish cells if there is many in current world.
         * @param cellSize set cell borders. std::numeric_limits<int>::max() disables cell borders.
         * @param shift set global shift of cell center.
         * @return true if there was no water at given cell if cellSize != std::numeric_limits<int>::max() or there is
         * at least single object is added to the scene, false if there is already water for given cell or there is no
         * any other objects.
         */
        virtual bool addWater(const osg::Vec2i& cellPosition, int cellSize, float level) = 0;

        /**
         * @brief removeWater to make it no more available at the scene.
         * @param cellPosition allows to find cell.
         * @return true if there was water at given cell.
         */
        virtual bool removeWater(const osg::Vec2i& cellPosition) = 0;

        virtual bool addHeightfield(const osg::Vec2i& cellPosition, int cellSize, const HeightfieldShape& shape) = 0;

        virtual bool removeHeightfield(const osg::Vec2i& cellPosition) = 0;

        virtual void addPathgrid(const ESM::Cell& cell, const ESM::Pathgrid& pathgrid) = 0;

        virtual void removePathgrid(const ESM::Pathgrid& pathgrid) = 0;

        /**
         * @brief update starts background navmesh update using current scene state.
         * @param playerPosition setup initial point to order build tiles of navmesh.
         */
        virtual void update(const osg::Vec3f& playerPosition) = 0;

        /**
         * @brief updatePlayerPosition starts background navmesh update using current scene state only when player position has been changed.
         * @param playerPosition setup initial point to order build tiles of navmesh.
         */
        virtual void updatePlayerPosition(const osg::Vec3f& playerPosition) = 0;

        /**
         * @brief disable navigator updates
         */
        virtual void setUpdatesEnabled(bool enabled) = 0;

        /**
         * @brief wait locks thread until tiles are updated from last update call based on passed condition type.
         * @param waitConditionType defines when waiting will stop
         */
        virtual void wait(Loading::Listener& listener, WaitConditionType waitConditionType) = 0;

        /**
         * @brief getNavMesh returns navmesh for specific agent half extents
         * @return navmesh
         */
        virtual SharedNavMeshCacheItem getNavMesh(const AgentBounds& agentBounds) const = 0;

        /**
         * @brief getNavMeshes returns all current navmeshes
         * @return map of agent half extents to navmesh
         */
        virtual std::map<AgentBounds, SharedNavMeshCacheItem> getNavMeshes() const = 0;

        virtual const Settings& getSettings() const = 0;

        virtual void reportStats(unsigned int frameNumber, osg::Stats& stats) const = 0;

        virtual RecastMeshTiles getRecastMeshTiles() const = 0;

        virtual float getMaxNavmeshAreaRealRadius() const = 0;
    };

    std::unique_ptr<Navigator> makeNavigator(const Settings& settings, const std::string& userDataPath);

    std::unique_ptr<Navigator> makeNavigatorStub();
}

#endif
