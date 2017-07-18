///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2012-2017 DreamWorks Animation LLC
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DreamWorks Animation nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////

/// @author Nick Avramoussis, Francisco Gochez
///
/// @file PointDelete.h
///
/// @brief Methods for deleting points based on group membership
///

#ifndef OPENVDB_POINTS_POINT_DELETE_HAS_BEEN_INCLUDED
#define OPENVDB_POINTS_POINT_DELETE_HAS_BEEN_INCLUDED

#include "PointDataGrid.h"
#include "PointGroup.h"
#include "IndexIterator.h"
#include "IndexFilter.h"

#include <openvdb/tree/LeafManager.h>

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace points {


/// @brief   Delete points that are members of specific groups
///
/// @details This method will delete points which are members of any of the supplied groups and
///          drop the groups from the tree. Optionally an invert flag can be used to delete
///          points that belong to none of the groups.
///
/// @param   pointTree    the point tree
/// @param   groups       the groups from which to delete points
/// @param   invert       if enabled, points not belonging to any of the groups will be deleted
///
/// @note    If the invert flag is true, none of the groups will be dropped after deleting points.

template <typename PointDataTree>
inline void deleteFromGroups(PointDataTree& pointTree, const std::vector<std::string>& groups,
                             bool invert = false);

/// @brief   Delete points that are members of a group
///
/// @details This method will delete points which are members of the supplied group and drop the
///          group from the tree. Optionally an invert flag can be used to delete
///          points that belong to none of the groups.
///
/// @param   pointTree    the point tree with the group to delete
/// @param   groups       the name of the group to delete
/// @param   invert       If this flag is set to true, points *not* in this group will be deleted
///
/// @note    If the invert flag is true, the group will not be dropped after deleting points.

template <typename PointDataTree>
inline void deleteFromGroup(PointDataTree& pointTree, const std::string& group,
                            bool invert = false);


////////////////////////////////////////


namespace point_delete_internal {


template <typename PointDataTreeT>
struct DeleteGroupsOp
{
    using LeafManagerT = tree::LeafManager<PointDataTreeT>;
    using LeafRangeT = typename LeafManagerT::LeafRange;
    using LeafNodeT = typename PointDataTreeT::LeafNodeType;
    using ValueType = typename LeafNodeT::ValueType;

    DeleteGroupsOp(const std::vector<std::string>& groupNames, bool invert)
        : mGroupNames(groupNames)
        , mInvert(invert) { }

    void operator()(const LeafRangeT& range) const
    {
        // based on the invert parameter reverse the include and exclude arguments

        std::unique_ptr<MultiGroupFilter> filter;
        if (mInvert) {
            filter.reset(new MultiGroupFilter(mGroupNames, std::vector<std::string>()));
        }
        else {
            filter.reset(new MultiGroupFilter(std::vector<std::string>(), mGroupNames));
        }

        for (auto leaf = range.begin(); leaf != range.end(); ++leaf)
        {
            // early-exit if the leaf has no points
            const size_t size = iterCount(leaf->beginIndexAll());
            if (size == 0)    continue;

            const size_t newSize =
                iterCount(leaf->template beginIndexAll<MultiGroupFilter>(*filter));

            // if all points are being deleted, clear the leaf attributes
            if (newSize == 0) {
                leaf->clearAttributes();
                continue;
            }

            const AttributeSet& existingAttributeSet = leaf->attributeSet();
            AttributeSet* newAttributeSet = new AttributeSet(existingAttributeSet, newSize);
            const size_t attributeSetSize = existingAttributeSet.size();

            // cache the attribute arrays for efficiency

            std::vector<AttributeArray*> newAttributeArrays;
            std::vector<const AttributeArray*> existingAttributeArrays;

            for (size_t i = 0; i < attributeSetSize; i++) {
                newAttributeArrays.push_back(newAttributeSet->get(i));
                existingAttributeArrays.push_back(existingAttributeSet.getConst(i));
            }

            size_t attributeIndex = 0;
            std::vector<ValueType> endOffsets;

            endOffsets.reserve(LeafNodeT::NUM_VALUES);

            // now construct new attribute arrays which exclude data from deleted points

            for (auto voxel = leaf->cbeginValueAll(); voxel; ++voxel) {
                for (auto iter = leaf->beginIndexVoxel(voxel.getCoord(), *filter);
                     iter; ++iter) {
                    for (size_t i = 0; i < attributeSetSize; i++) {
                        newAttributeArrays[i]->set(attributeIndex, *(existingAttributeArrays[i]),
                            *iter);
                    }
                    ++attributeIndex;
                }
                endOffsets.push_back(ValueType(attributeIndex));
            }

            leaf->replaceAttributeSet(newAttributeSet);
            leaf->setOffsets(endOffsets);
        }
    }

private:
    const std::vector<std::string>& mGroupNames;
    bool mInvert;
}; // struct DeleteGroupsOp

} // namespace point_delete_internal


////////////////////////////////////////


template <typename PointDataTreeT>
inline void deleteFromGroups(PointDataTreeT& pointTree, const std::vector<std::string>& groups,
    bool invert)
{
    const typename PointDataTreeT::LeafCIter leafIter = pointTree.cbeginLeaf();

    if (!leafIter)    return;

    const openvdb::points::AttributeSet& attributeSet = leafIter->attributeSet();
    const AttributeSet::Descriptor& descriptor = attributeSet.descriptor();
    std::vector<std::string> availableGroups;

    // determine which of the requested groups exist, and early exit
    // if none are present in the tree

    for (const auto& groupName : groups) {
        if (descriptor.hasGroup(groupName)) {
            availableGroups.push_back(groupName);
        }
    }

    if (availableGroups.empty())    return;

    tree::LeafManager<PointDataTreeT> leafManager(pointTree);
    point_delete_internal::DeleteGroupsOp<PointDataTreeT> deleteOp(availableGroups, invert);
    tbb::parallel_for(leafManager.leafRange(), deleteOp);

    // drop the now-empty groups (unless invert = true)

    if (!invert) {
        dropGroups(pointTree, availableGroups);
    }
}

template <typename PointDataTreeT>
inline void deleteFromGroup(PointDataTreeT& pointTree, const std::string& group, bool invert)
{
    std::vector<std::string> groups(1, group);

    deleteFromGroups(pointTree, groups, invert);
}


} // namespace points
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif // OPENVDB_POINTS_POINT_DELETE_HAS_BEEN_INCLUDED

// Copyright (c) 2012-2017 DreamWorks Animation LLC
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
