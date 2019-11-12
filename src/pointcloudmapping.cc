/*
 * <one line to give the program's name and a brief idea of what it does.>
 * Copyright (C) 2016  <copyright holder> <email>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "pointcloudmapping.h"
#include <KeyFrame.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/common/projection_matrix.h>
#include <pcl/io/pcd_io.h>
#include <pcl_conversions/pcl_conversions.h>
#include "Converter.h"
#include "ros/ros.h"
#include <sensor_msgs/PointCloud2.h>
#include <boost/make_shared.hpp>

#include <octomap/octomap.h>
#include <octomap/ColorOcTree.h>
#include <octomap_msgs/conversions.h>

PointCloudMapping::PointCloudMapping(float resolution_, Map *map) : mpMap(map)
{
    this->resolution = resolution_;
    voxel.setLeafSize(resolution, resolution, resolution);
    pass.setFilterFieldName("z");
    pass.setFilterLimits(0.0, 3.0);
    globalMap = boost::make_shared<PointCloud>();
    localMap = boost::make_shared<PointCloud>();

    viewerThread = make_shared<thread>(bind(&PointCloudMapping::viewer, this));
}

void PointCloudMapping::shutdown()
{
    {
        unique_lock<mutex> lck(shutDownMutex);
        shutDownFlag = true;
        keyFrameUpdated.notify_one();
    }
    viewerThread->join();
}

void PointCloudMapping::insertKeyFrame(KeyFrame *kf, cv::Mat &color, cv::Mat &depth)
{
    cout << kf->mTimeStamp << " receive a keyframe, id = " << kf->mnId << endl;
    unique_lock<mutex> lck(keyframeMutex);
    keyframes.push_back(kf);
    colorImgs.push_back(color.clone());
    depthImgs.push_back(depth.clone());

    keyFrameUpdated.notify_one();
}

void PointCloudMapping::insertKeyFrame(KeyFrame *kf)
{
    cout << kf->mTimeStamp << " receive a keyframe, id = " << kf->mnId << endl;
    unique_lock<mutex> lck(keyframeMutex);

    keyFrameUpdated.notify_one();
}

pcl::PointCloud<PointCloudMapping::PointT>::Ptr
PointCloudMapping::generatePointCloud(KeyFrame *kf, cv::Mat &color, cv::Mat &depth)
{
    PointCloud::Ptr tmp(new PointCloud());
    // point cloud is null ptr
    for (int m = 0; m < depth.rows; m += 3)
    {
        for (int n = 0; n < depth.cols; n += 3)
        {
            float d = depth.ptr<float>(m)[n];
            if (d < 0.01 || d > 10)
                continue;
            PointT p;
            p.z = d;
            p.x = (n - kf->cx) * p.z / kf->fx;
            p.y = (m - kf->cy) * p.z / kf->fy;

//            p.rgb = 0.1;


            p.b = color.ptr<uchar>(m)[n * 3];
            p.g = color.ptr<uchar>(m)[n * 3 + 1];
            p.r = color.ptr<uchar>(m)[n * 3 + 2];
            p.a = 0;

            tmp->points.push_back(p);
        }
    }

    Eigen::Isometry3d T = ORB_SLAM2::Converter::toSE3Quat(kf->GetPose());
    PointCloud::Ptr cloud(new PointCloud);
    pass.setInputCloud(tmp);
    pass.filter(*cloud);
    tmp->swap(*cloud);

    pcl::transformPointCloud(*tmp, *cloud, T.inverse().matrix());
    cloud->is_dense = false;

    cout << "generate point cloud for kf " << kf->mnId << ", size=" << cloud->points.size() << endl;
    return cloud;
}


void PointCloudMapping::viewer()
{
//    pcl::visualization::CloudViewer viewer("PointCloud Viewer");
    ros::NodeHandle nh;
    octomap::ColorOcTree* colortree = new octomap::ColorOcTree(0.03);
    pubLaserCloudlocalMap = nh.advertise<sensor_msgs::PointCloud2>("/localmap", 100);
    pub_octomap = nh.advertise<octomap_msgs::Octomap>("/local_octomap", 100, true);
    ros::Rate rate(100);
    while (ros::ok())
    {
        sensor_msgs::PointCloud2 localmap_ros;

        {
            unique_lock<mutex> lck_shutdown(shutDownMutex);
            if (shutDownFlag)
            {
                break;
            }
        }
        {
            unique_lock<mutex> lck_keyframeUpdated(keyFrameUpdateMutex);
            keyFrameUpdated.wait(lck_keyframeUpdated);
        }


        // keyframe is updated
//        keyframes = mpMap->GetAllKeyFrames();
//        size_t N = 0;
//        {
//            unique_lock<mutex> lck(keyframeMutex);
//            N = keyframes.size();
//        }
//
//        for (size_t i = lastKeyframeSize; i < N; i++)
//        {
//            PointCloud::Ptr p = generatePointCloud(keyframes[i], keyframes[i]->img0, keyframes[i]->img1);
//            *globalMap += *p;
//        }
//
//        PointCloud::Ptr tmp(new PointCloud());
//        voxel.setInputCloud(globalMap);
//        voxel.filter(*tmp);
//
//        globalMap->swap(*tmp);

//        generateglobalMap();
//        viewer.showCloud(globalMap);
//        cout << "show global map, size=" << globalMap->points.size() << endl;

        generatelocalMap();

        for(auto p:localMap->points)
        {
            colortree->updateNode(octomap::point3d(p.x, p.y, p.z), true);
        }

        for(auto p:localMap->points)
        {
            colortree->integrateNodeColor(p.x, p.y, p.z, p.r, p.g, p.b);
        }

        octomap_msgs::Octomap map_msg;
        map_msg.header.frame_id = "camera_link";
        map_msg.header.stamp = ros::Time::now();
        if (octomap_msgs::fullMapToMsg(*colortree, map_msg))
            pub_octomap.publish(map_msg);

        pcl::toROSMsg(*localMap, localmap_ros);
        localmap_ros.header.frame_id = "/camera_link";
        pubLaserCloudlocalMap.publish(localmap_ros);
//        viewer.showCloud(localMap);

        ros::spinOnce();
        rate.sleep();

        cout << "show local map, size=" << localMap->points.size() << endl;
        savePointCloud("result.pcd");
//        lastKeyframeSize = N;
    }

    cout << "pcl_viewer shutdown!" << endl;
}

void PointCloudMapping::savePointCloud(const string &name)
{
//    generateMap();
    if (!localMap->empty())
    {
        pcl::io::savePCDFileBinary(name, *localMap);
        cout << "save map to file : " << name << endl;
    }
}

void PointCloudMapping::generatelocalMap()
{
    keyframes = mpMap->GetAllKeyFrames();
    localMap->clear();
    size_t N = 0;
    {
        unique_lock<mutex> lck(keyframeMutex);
        N = keyframes.size();
    }
    cout<<"keyframes number: "<<N<<endl;

    if (N <= 30)
    {
        for (size_t i = 0; i < N; i++)
        {
            PointCloud::Ptr p = generatePointCloud(keyframes[i], keyframes[i]->img0, keyframes[i]->img1);
            *localMap += *p;
        }
    }
    else
    {
        for (size_t i = N - 2; i > N - 31; i--)
        {
            PointCloud::Ptr p = generatePointCloud(keyframes[i], keyframes[i]->img0, keyframes[i]->img1);
            *localMap += *p;
        }
    }

    PointCloud::Ptr tmp(new PointCloud());
    voxel.setInputCloud(localMap);
    voxel.filter(*tmp);

    localMap->swap(*tmp);

    cout << "show local map, size=" << localMap->points.size() << endl;

}

void PointCloudMapping::generateglobalMap()
{
    keyframes = mpMap->GetAllKeyFrames();
    globalMap->clear();
    size_t N = 0;
    {
        unique_lock<mutex> lck(keyframeMutex);
        N = keyframes.size();
    }

    for (size_t i = 0; i < N; i++)
    {
        PointCloud::Ptr p = generatePointCloud(keyframes[i], keyframes[i]->img0, keyframes[i]->img1);
        *globalMap += *p;
    }

    PointCloud::Ptr tmp(new PointCloud());
    voxel.setInputCloud(globalMap);
    voxel.filter(*tmp);

    globalMap->swap(*tmp);

    cout << "show global map, size=" << globalMap->points.size() << endl;

}
