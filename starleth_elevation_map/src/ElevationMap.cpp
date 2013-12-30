/*
 * ElevationMap.cpp
 *
 *  Created on: Nov 12, 2013
 *      Author: Péter Fankhauser
 *	 Institute: ETH Zurich, Autonomous Systems Lab
 */
#include "ElevationMap.hpp"

// StarlETH Navigation
#include <starleth_elevation_msg/ElevationMap.h>
#include <EigenConversions.hpp>

// PCL
#include <pcl/common/transforms.h>
#include <pcl/filters/passthrough.h>

// ROS
#include <tf_conversions/tf_eigen.h>

using namespace std;
using namespace Eigen;
using namespace pcl;
using namespace ros;
using namespace tf;

namespace starleth_elevation_map {

ElevationMap::ElevationMap(ros::NodeHandle& nodeHandle)
    : nodeHandle_(nodeHandle)
{
  ROS_INFO("StarlETH elevation map node started.");
  readParameters();
  pointCloudSubscriber_ = nodeHandle_.subscribe(pointCloudTopic_, 1, &ElevationMap::pointCloudCallback, this);
  elevationMapPublisher_ = nodeHandle_.advertise<starleth_elevation_msg::ElevationMap>("elevation_map", 1);
  resize(lengthInX_, lengthInY_);
}

ElevationMap::~ElevationMap()
{

}

bool ElevationMap::readParameters()
{
  nodeHandle_.param("point_cloud_topic", pointCloudTopic_, string("/depth_registered/points_throttled"));
  nodeHandle_.param("map_frame_id", parentFrameId_, string("/map"));
  nodeHandle_.param("elevation_map_id", elevationMapFrameId_, string("/elevation_map"));
  nodeHandle_.param("sensor_cutoff_depth", sensorCutoffDepth_, 3.0);
  nodeHandle_.param("elevation_map_length_in_x", lengthInX_, 1.25);
  nodeHandle_.param("elevation_map_length_in_y", lengthInY_, 0.75);
  nodeHandle_.param("elevation_map_resolution", resolution_, 0.25);
  ROS_ASSERT(resolution_ > 0.0);
  elevationMapToParentTransform_.setIdentity();

  return true;
}

void ElevationMap::pointCloudCallback(
    const sensor_msgs::PointCloud2& rawPointCloud)
{
  if (elevationMapPublisher_.getNumSubscribers () < 1) return;

  // Convert the sensor_msgs/PointCloud2 data to pcl/PointCloud
  PointCloud<PointXYZRGB>::Ptr pointCloud(new PointCloud<PointXYZRGB>);
  fromROSMsg(rawPointCloud, *pointCloud);

  int pointCloudSize = pointCloud->width * pointCloud->height;
  ROS_DEBUG("ElevationMap received a point cloud (%i points) for elevation mapping.", pointCloudSize);

  if (!broadcastElevationMapTransform(pointCloud->header.stamp))
  {
    ROS_ERROR("ElevationMap: Broadcasting elevation map transform to parent failed.");
    return;
  }

  cleanPointCloud(pointCloud);

  if (!transformPointCloud(pointCloud, elevationMapFrameId_))
  {
    ROS_ERROR("ElevationMap: Point cloud transform failed.");
    return;
  }

  if (!addToElevationMap(pointCloud))
  {
    ROS_ERROR("ElevationMap: Adding point cloud to elevation map failed.");
    return;
  }

  if (!publishElevationMap())
  {
    ROS_ERROR("ElevationMap: Broadcasting elevation map failed.");
    return;
  }

  setTimeOfLastUpdate(pointCloud->header.stamp);
}

bool ElevationMap::cleanPointCloud(const PointCloud<PointXYZRGB>::Ptr pointCloud)
{
  PassThrough<PointXYZRGB> passThroughFilter;
  PointCloud<PointXYZRGB> tempPointCloud;

  passThroughFilter.setInputCloud(pointCloud);
  passThroughFilter.setFilterFieldName("z");
  passThroughFilter.setFilterLimits(0.0, sensorCutoffDepth_);
  passThroughFilter.filter(tempPointCloud);
  tempPointCloud.is_dense = true;
  pointCloud->swap(tempPointCloud);

  int pointCloudSize = pointCloud->width * pointCloud->height;
  ROS_DEBUG("ElevationMap cleanPointCloud() reduced point cloud to %i points.", pointCloudSize);
}

bool ElevationMap::transformPointCloud(
    const PointCloud<PointXYZRGB>::Ptr pointCloud,
    const std::string& targetFrame)
{
  StampedTransform transformTf;
  string sourceFrame = pointCloud->header.frame_id;
  Time timeStamp =  pointCloud->header.stamp;

  PointCloud<PointXYZRGB>::Ptr pointCloudTransformed(new PointCloud<PointXYZRGB>);

  try
  {
    transformListener_.waitForTransform(targetFrame, sourceFrame, timeStamp, ros::Duration(1.0));
    transformListener_.lookupTransform(targetFrame, sourceFrame, timeStamp, transformTf);
    Affine3d transform;
    poseTFToEigen(transformTf, transform);
    pcl::transformPointCloud(*pointCloud, *pointCloudTransformed, transform.cast<float>());
    pointCloud->swap(*pointCloudTransformed);
    pointCloud->header.frame_id = targetFrame;
//    pointCloud->header.stamp = timeStamp;
    return true;
  }
  catch (TransformException &ex)
  {
    ROS_ERROR("%s", ex.what());
    return false;
  }
}

bool ElevationMap::publishElevationMap()
{
  starleth_elevation_msg::ElevationMap elevationMapMessage_;

  elevationMapMessage_.header.stamp = timeOfLastUpdate_;
  elevationMapMessage_.header.frame_id = elevationMapFrameId_;
  elevationMapMessage_.resolution = resolution_;
  elevationMapMessage_.lengthInX = lengthInX_;
  elevationMapMessage_.lengthInY = lengthInY_;

  starleth_elevation_msg::matrixEigenToMultiArrayMessage(elevationData_, elevationMapMessage_.elevation);
  starleth_elevation_msg::matrixEigenToMultiArrayMessage(varianceData_, elevationMapMessage_.variance);

  elevationMapPublisher_.publish(elevationMapMessage_);

  return true;
}

bool ElevationMap::broadcastElevationMapTransform(ros::Time time)
{
  tf::Transform tfTransform;
  poseEigenToTF(elevationMapToParentTransform_, tfTransform);
  transformBroadcaster_.sendTransform(tf::StampedTransform(tfTransform, time, parentFrameId_, elevationMapFrameId_));
  ROS_DEBUG("Published transform for elevation map in parent frame at time %f.", time.toSec());
  return true;
}

bool ElevationMap::addToElevationMap(
    const pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointCloud)
{
elevationData_.setZero();
//  elevationData_ *= 0.05;
//
////  elevationData_(0,0) = 0.1;
//  elevationData_.bottomRightCorner(1,1).setConstant(0.2);
//
//  elevationData_.topRightCorner(1,1).setConstant(-0.2);

  elevationData_.bottomRightCorner(1,1).setConstant(NAN);

  cout << elevationData_ << endl;

//  bool local_map_is_subscribed = (pub_local_map.getNumSubscribers () > 0);
//  bool global_map_is_subscribed = (pub_global_map.getNumSubscribers () > 0);
//
//  if(local_map_is_subscribed)
//      local_elevation_map.data = std::vector<int16_t>(elevation_map_meta.width * elevation_map_meta.height,(int16_t)-elevation_map_meta.zero_elevation);
//
//  unsigned int size = (unsigned int)pointcloud2_map_pcl->points.size();
//
//  // iterate trough all points
//  for (unsigned int k = 0; k < size; ++k)
//  {
//      const pcl::PointXYZ& pt_cloud = pointcloud2_map_pcl->points[k];
//
//      double measurement_distance = pointcloud2_sensor_pcl->points[k].z;
//
//      // check for invalid measurements
//      if (isnan(pt_cloud.x) || isnan(pt_cloud.y) || isnan(pt_cloud.z))
//          continue;
//
//      // check max distance (manhatten norm)
//      if(max_observable_distance < measurement_distance)
//          continue;
//
//      // check min/max height
//      if(elevation_map_meta.min_elevation+local_map_transform.getOrigin().z() > pt_cloud.z || elevation_map_meta.max_elevation+local_map_transform.getOrigin().z() < pt_cloud.z)
//          continue;
//
//      // allign grid points
//      Eigen::Vector2f index_world(pt_cloud.x, pt_cloud.y);
//      Eigen::Vector2f index_map (world_map_transform.getC2Coords(index_world));
//
//      unsigned int cell_index = MAP_IDX(elevation_map_meta.width, (int)round(index_map(0)), (int)round(index_map(1)));
//
//      int16_t* pt_local_map = &local_elevation_map.data[cell_index];
//      int16_t* pt_global_map = &global_elevation_map.data[cell_index];
//      double*  pt_var = &cell_variance[cell_index];
//
//
//      if(local_map_is_subscribed)
//      {
//          // elevation in current cell in meter
//          double cell_elevation = elevation_map_meta.resolution_z*(*pt_local_map-elevation_map_meta.zero_elevation);
//
//          // store maximum of each cell
//          if(pt_cloud.z > cell_elevation)
//              *pt_local_map = (int16_t)(round(pt_cloud.z/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//
//          // filter each cell localy
////            double measurement_variance = sensor_variance*(measurement_distance*measurement_distance);
////            if(*pt_local_map == (int16_t)-elevation_map_meta.zero_elevation)
////            {
////                // unknown cell -> use current measurement
////                *pt_local_map = (int16_t)(round(pt_cloud.z/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
////                *pt_var = measurement_variance;
////            }
////            else
////            {
////                // fuse cell_elevation with measurement
////                *pt_local_map = (int16_t) (round(((measurement_variance * cell_elevation + *pt_var * pt_cloud.z)/(*pt_var + measurement_variance))/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
////                *pt_var = (measurement_variance * *pt_var)/(measurement_variance + *pt_var);
////            }
//      }
//
//      if(publish_poseupdate || global_map_is_subscribed)
//      {
//          // fuse new measurements with existing map
//
//          // elevation in current cell in meter
//          double cell_elevation = elevation_map_meta.resolution_z*(*pt_global_map-elevation_map_meta.zero_elevation);
//
//          // measurement variance
//          double measurement_variance = sensor_variance*(measurement_distance*measurement_distance);
//
//          // mahalanobis distance
//          double mahalanobis_distance = sqrt((pt_cloud.z - cell_elevation)*(pt_cloud.z - cell_elevation)/(measurement_variance*measurement_variance));
//
//          if(pt_cloud.z > cell_elevation && (mahalanobis_distance > 5.0))
//          {
//              *pt_global_map = (int16_t)(round(pt_cloud.z/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//              *pt_var = measurement_variance;
//              continue;
//          }
//
//          if((pt_cloud.z < cell_elevation) && (mahalanobis_distance > 5.0))
//          {
//              *pt_global_map = (int16_t) (round(((measurement_variance * cell_elevation + *pt_var * pt_cloud.z)/(*pt_var + measurement_variance))/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//              //*pt_var = (measurement_variance * *pt_var)/(measurement_variance + *pt_var);
//              *pt_var = measurement_variance;
//              continue;
//          }
//
//          *pt_global_map = (int16_t) (round(((measurement_variance * cell_elevation + *pt_var * pt_cloud.z)/(*pt_var + measurement_variance))/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//          *pt_var = (measurement_variance * *pt_var)/(measurement_variance + *pt_var);
//      }
//  }
//
//
//  if(local_map_is_subscribed)
//  {
//      // set the header information on the map
//      local_elevation_map.header.stamp = pointcloud2_sensor_msg->header.stamp;
//      local_elevation_map.header.frame_id = map_frame_id;
//
//      pub_local_map.publish(local_elevation_map);
//  }
//
//  if(global_map_is_subscribed)
//  {
//      // set the header information on the map
//      global_elevation_map.header.stamp = pointcloud2_sensor_msg->header.stamp;
//      global_elevation_map.header.frame_id = map_frame_id;
//
//      pub_global_map.publish(global_elevation_map);
//  }



  return true;
}

bool ElevationMap::resize(double lengthInX, double lengthInY)
{
  lengthInX_ = lengthInX;
  lengthInY_ = lengthInY;

  int nRows = static_cast<int>(lengthInX_ / resolution_);
  int nCols = static_cast<int>(lengthInY_ / resolution_);
  elevationData_.resize(nRows, nCols);
  varianceData_.resize(nRows, nCols);

  ROS_DEBUG_STREAM("Elevation map matrix resized to " << elevationData_.rows() << " rows and "  << elevationData_.cols() << " columns.");
}

void ElevationMap::setTimeOfLastUpdate(const ros::Time& timeOfLastUpdate)
{
  timeOfLastUpdate_ = timeOfLastUpdate;
}

} /* namespace starleth_elevation_map */