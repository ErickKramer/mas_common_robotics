#include <ros/ros.h>
#include <ros/console.h>
#include <ros/topic.h>

#include <pcl_conversions/pcl_conversions.h>

#include <pcl/PCLPointCloud2.h>
#include <pcl/conversions.h>
#include <pcl/filters/passthrough.h>

#include <mcr_perception_msgs/FindWorkspace.h>

#include "mcr_scene_segmentation/planar_polygon_visualizer.h"
#include "mcr_scene_segmentation/impl/helpers.hpp"
#include "mcr_scene_segmentation/plane_extraction.h"
#include "mcr_scene_segmentation/polyclipping.h"

using namespace mcr::visualization;

/** This node provides a service to find a workspace in the data coming from
  * a Kinect/Asus camera.
  *
  * Here "workspace" means a planar polygon. Since there might be multiple
  * planar surfaces in the scene, the workspace is considered to be the one
  * with the largest area.
  *
  * In order to avoid detection of floor as a workspace, additional constraints
  * (normal orientation and plane elevation) could be supplied via node
  * parameters.
  *
  * Provides services:
  *   1) "find_workspace"
  *
  * Publishes:
  *   1) "workspace_polygon"
  *      An RViz marker that visualizes the polygon that defines the detected
  *      workspace.
  *
  * Subscribes:
  *   1) "/camera/depth_registered/points"
  *      The subscription is activated on demand, i.e. when the service is idle
  *      the node unsubscribes to avoid bandwidth consumption. */
class WorkspaceFinderNode
{

public:

  WorkspaceFinderNode()
  : polygon_visualizer_("workspace_polygon", Color::SALMON)
  {
    ros::NodeHandle nh("~");
    find_workspace_server_ = nh.advertiseService("find_workspace", &WorkspaceFinderNode::findWorkspaceCallback, this);
    ROS_INFO("Started [find_workspace] service.");
    plane_extraction_.setSortByArea(true);
  }

private:

  bool findWorkspaceCallback(mcr_perception_msgs::FindWorkspace::Request& request, mcr_perception_msgs::FindWorkspace::Response& response)
  {
    ros::NodeHandle nh("~");
    ROS_INFO("Received [find_workspace] request.");
    updateConfiguration();

    ROS_INFO("Waiting for a point cloud message...");
    auto ros_cloud = ros::topic::waitForMessage<sensor_msgs::PointCloud2>("input_pointcloud", nh, ros::Duration(cloud_timeout_));
    if (!ros_cloud)
    {
      ROS_ERROR("No point cloud messages during last %i seconds, aborting.", cloud_timeout_);
      return false;
    }

    // Prepare point cloud: convert from ROS message and run pasthrough filter.
    PointCloud::Ptr cloud(new PointCloud);
    PointCloud::Ptr cloud_filtered(new PointCloud);
    pcl::PCLPointCloud2 pc2;
    pcl_conversions::toPCL(*ros_cloud, pc2);
    pcl::fromPCLPointCloud2(pc2, *cloud);
    pass_through_->setInputCloud(cloud);
    pass_through_->filter(*cloud_filtered);

    PlanarPolygonVector planar_polygons;
    plane_extraction_.setInputCloud(cloud_filtered);
    plane_extraction_.extract(planar_polygons);
    ROS_INFO("Plane extraction found %zu planar polygons.", planar_polygons.size());

    if (!planar_polygons.size())
      return false;

    // The first polygon in the output vector has the largest area, so we take it.
    auto& polygon = planar_polygons[0];
    // Before sending it to the user, we clip the polygon.
    // Sometimes it fails due to wrong orientation of polygon faces.
    try
    {
      clipPlanarPolygon(polygon, clip_polygon_by_);
    }
    catch(std::exception& e)
    {
      ROS_ERROR("Failed to clip polygon (%s), output original unclipped one.", e.what());
    }
    response.stamp = ros_cloud->header.stamp;
    convertPlanarPolygon(polygon, response.polygon);
    response.polygon.contour.push_back(response.polygon.contour.front());
    polygon_visualizer_.publish(polygon, ros_cloud->header.frame_id);
    return true;
  }

  void updateConfiguration()
  {
    ros::NodeHandle pn("~");

    // Passthrough filter
    pass_through_.reset(new pcl::PassThrough<PointT>);
    pass_through_->setKeepOrganized(true);
    double min_x, max_x;
    if (pn.getParam("min_x", min_x) && pn.getParam("max_x", max_x))
    {
      pass_through_->setFilterFieldName("x");
      pass_through_->setFilterLimits(min_x, max_x);
    }
    double min_y, max_y;
    if (pn.getParam("min_y", min_y) && pn.getParam("max_y", max_y))
    {
      pass_through_->setFilterFieldName("y");
      pass_through_->setFilterLimits(min_y, max_y);
    }
    double min_z, max_z;
    if (pn.getParam("min_z", min_z) && pn.getParam("max_z", max_z))
    {
      pass_through_->setFilterFieldName("z");
      pass_through_->setFilterLimits(min_z, max_z);
    }

    // Plane constraints
    double normal_x, normal_y, normal_z;
    double distance;
    if (pn.getParam("normal_x", normal_x) && pn.getParam("normal_y", normal_y) && pn.getParam("normal_z", normal_z))
    {
      Eigen::Vector3f normal(normal_x, normal_y, normal_z);
      normal.normalize();
      if (pn.getParam("distance", distance))
        plane_extraction_.setPlaneConstraints(normal, pcl::deg2rad(20.0f), distance, 0.05);
      else
        plane_extraction_.setPlaneConstraints(normal, pcl::deg2rad(20.0f));
    }
    else
    {
      plane_extraction_.removePlaneConstraints();
    }

    // Other settings
    pn.param("cloud_timeout", cloud_timeout_, 15);
    pn.param("clip_polygon_by", clip_polygon_by_, 0.03);
  }

  PlaneExtraction plane_extraction_;
  std::unique_ptr<pcl::PassThrough<PointT>> pass_through_;

  ros::ServiceServer find_workspace_server_;

  std::string cloud_topic_;
  int cloud_timeout_;
  double clip_polygon_by_;

  PlanarPolygonVisualizer polygon_visualizer_;

};

int main(int argc, char** argv)
{
  ros::init(argc, argv, "workspace_finder");
  WorkspaceFinderNode wfn;
  ros::spin();
  return 0;
}

