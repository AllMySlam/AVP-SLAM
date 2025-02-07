#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <nav_msgs/Odometry.h>
#include <opencv2/opencv.hpp>
#include <pcl/common/common.h>
#include <pcl/common/transforms.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/range_image/range_image.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/ndt.h>
#include <pcl/search/impl/search.hpp>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl_conversions/pcl_conversions.h>
#include <ros/ros.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/Int32.h>
#include <std_msgs/String.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>

#include <cmath>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>

typedef pcl::PointXYZRGB PointType;

int main(int argc, char* argv[])
{
    ros::init(argc, argv, "bag_it");
    ros::NodeHandle nh;

    std::string fileName;

    double icpFitnessScoreThresh = 0.01;
    double closeThresh           = 0.01;

    if (argc >= 2) {
        fileName = argv[1];
    }
    else {
        std::cout << "argument missing: file name" << std::endl;
        return 1;
    }
    if (argc >= 3) {
        icpFitnessScoreThresh = std::stod(argv[2]);
        closeThresh           = std::stod(argv[2]);
    }

    std::string     valuableTopic = "/currentFeatureInWorld";
    Eigen::Affine3f transWorldCurrent;

    ros::Publisher pubCloudI = nh.advertise<sensor_msgs::PointCloud2>("/cloudI", 100);
    ros::Publisher pubCloudJ = nh.advertise<sensor_msgs::PointCloud2>("/cloudJ", 100);

    std::vector<pcl::PointCloud<PointType>::Ptr> pointClouds;
    std::vector<int>                             pointCloudsTime;
    std::vector<double>                          pointCloudsX;
    std::vector<double>                          pointCloudsY;
    std::vector<double>                          pointCloudsTheta;

    rosbag::Bag bag;
    std::string dataDir = "/home/catkin_ws/src/AVP-SLAM-PLUS/data/rosbag/";
    std::string outDir  = "/home/catkin_ws/src/AVP-SLAM-PLUS/data/g2o/";

    bag.open(dataDir + fileName + ".bag", rosbag::bagmode::Read);

    // file to save the vertex and edges to dedicated g2o file
    std::ofstream myfile;
    myfile.open(outDir + fileName + ".g2o");

    // possible to look at point clouds?
    // pcl::visualization::CloudViewer viewer("Cloud Viewer");
    // viewer.showCloud(cloud);

    float currentX     = 0;
    float currentY     = 0;
    float currentZ     = 0;
    float currentRoll  = 0;
    float currentPitch = 0;
    float currentYaw   = 0;

    int vertexCount = 0;
    int edgeCount   = 0;

    for (rosbag::MessageInstance const m : rosbag::View(bag)) {
        std::string topic = m.getTopic();
        // ros::Time time = m.getTime();

        std_msgs::String::ConstPtr s = m.instantiate<std_msgs::String>();
        if (s != NULL) {
            std::string text = s->data;
            // myfile << text << std::endl;

            size_t      pos  = text.find(" ");
            std::string name = text.substr(0, pos);
            if (name == "VERTEX_SE2") {
                text = text.erase(0, pos + 1);
                pos  = text.find(" ");
                // ignore time
                text = text.erase(0, pos + 1);
                myfile << name << " " << vertexCount << " " << text << std::endl;

                // trim x
                pos      = text.find(" ");
                currentX = std::stod(text.substr(0, pos));
                text     = text.erase(0, pos + 1);
                // trim y
                pos      = text.find(" ");
                currentY = std::stod(text.substr(0, pos));
                text     = text.erase(0, pos + 1);
                // save yaw
                currentYaw = std::stod(text);

                vertexCount++;
            }
            else if (name == "EDGE_SE2" && vertexCount > 0) {
                text = text.erase(0, pos + 1);
                pos  = text.find(" ");
                // ignore time 1
                text = text.erase(0, pos + 1);
                pos  = text.find(" ");
                // ignore time 2
                text = text.erase(0, pos + 1);
                myfile << name << " " << edgeCount << " " << edgeCount + 1 << " " << text << std::endl;
                edgeCount++;
            }
        }

        sensor_msgs::PointCloud2::ConstPtr input = m.instantiate<sensor_msgs::PointCloud2>();
        if (input != nullptr && vertexCount != 0) {
            pcl::PointCloud<PointType>::Ptr cloud(new pcl::PointCloud<PointType>);
            pcl::fromROSMsg(*input, *cloud);

            pointClouds.push_back(cloud);
            pointCloudsTime.push_back(vertexCount - 1);
            pointCloudsX.push_back(currentX);
            pointCloudsY.push_back(currentY);
            pointCloudsTheta.push_back(currentYaw);
        }
    }

    bag.close();
    std::cout << "done reading " << pointClouds.size() << " clouds" << std::endl;

    int ignoredClouds  = 500;
    int cloudIncrement = 1;
    int newEdgeCount   = 0;

    static pcl::IterativeClosestPoint<PointType, PointType> icp;
    icp.setMaxCorrespondenceDistance(20);
    icp.setMaximumIterations(100);
    icp.setTransformationEpsilon(1e-10);
    icp.setEuclideanFitnessEpsilon(0.001);

    for (int j = ignoredClouds; j < pointClouds.size(); j += cloudIncrement) {
        if (pointCloudsX[j] < 0.5 || pointCloudsX[j] > 1.5 || pointCloudsY[j] < -0.5 || pointCloudsY[j] > 0.5) {
            continue;
        }
        std::cout << "Trying " << j << "/" << pointClouds.size() << "\r";
        // j should be all clouds with a possible transformation from previous clouds i
        // std::cout << pointCloudsTime[j] << std::endl;
        pcl::PointCloud<PointType>::Ptr cloud_j = pointClouds[j];

        // transform cloud to map orientation
        pcl::PointCloud<PointType>::Ptr transformed_cloud_j(new pcl::PointCloud<PointType>());
        Eigen::Affine3f                 transform = Eigen::Affine3f::Identity();
        transform.rotate(Eigen::AngleAxisf(pointCloudsTheta[j], Eigen::Vector3f::UnitZ()));
        pcl::transformPointCloud(*cloud_j, *transformed_cloud_j, transform);

        icp.setInputTarget(transformed_cloud_j);

        for (int i = 0; i < j - ignoredClouds; i += cloudIncrement) {
            // save time by ignoring far comparisons
            double tooFar = 2;
            if (pow((pointCloudsX[i] - pointCloudsX[j]), 2) + pow((pointCloudsY[i] - pointCloudsY[j]), 2) >
                pow(tooFar, 2)) {
                continue;
            }

            // compare the current cloud(j) to all past clouds(i)
            pcl::PointCloud<PointType>::Ptr cloud_i = pointClouds[i];

            // transform cloud to map orientation
            pcl::PointCloud<PointType>::Ptr transformed_cloud_i(new pcl::PointCloud<PointType>());
            transform = Eigen::Affine3f::Identity();
            transform.rotate(Eigen::AngleAxisf(pointCloudsTheta[i], Eigen::Vector3f::UnitZ()));
            pcl::transformPointCloud(*cloud_i, *transformed_cloud_i, transform);

            icp.setInputSource(transformed_cloud_i);

            pcl::PointCloud<PointType>::Ptr transCurrentCloudInWorld(new pcl::PointCloud<PointType>());
            icp.align(*transCurrentCloudInWorld);

            double icpScore = icp.getFitnessScore();

            // if the score is good then find the transformation
            if (icp.hasConverged() == false || icpScore > icpFitnessScoreThresh) {
                // std::cout << "ICP locolization failed the score is " << icpScore << std::endl;
            }
            else {
                // std::cout<<"passed "<<i<<" "<<j<<std::endl;
                transWorldCurrent = icp.getFinalTransformation();
                pcl::getTranslationAndEulerAngles(transWorldCurrent, currentX, currentY, currentZ, currentRoll,
                                                  currentPitch, currentYaw);
                if (currentX < closeThresh && currentY < closeThresh && currentZ < closeThresh &&
                    currentRoll < closeThresh && currentPitch < closeThresh && currentYaw < closeThresh) {
                    myfile << "EDGE_SE2 " << pointCloudsTime[i] << " " << pointCloudsTime[j] << " " << currentX << " "
                           << currentY << " " << currentYaw << " 10000 1000 1000 10000 1000 10000" << std::endl;
                    newEdgeCount++;

                    std::cout << "matched " << i << " at " << pointCloudsX[i] << "," << pointCloudsY[i] << " and " << j
                              << " at " << pointCloudsX[j] << "," << pointCloudsY[j] << std::endl;

                    sensor_msgs::PointCloud2 cameraCloudFrameMsg;
                    pcl::toROSMsg(*transformed_cloud_i, cameraCloudFrameMsg);
                    cameraCloudFrameMsg.header.stamp    = ros::Time::now();
                    cameraCloudFrameMsg.header.frame_id = "/camera0_link";
                    pubCloudI.publish(cameraCloudFrameMsg);

                    pcl::toROSMsg(*transformed_cloud_j, cameraCloudFrameMsg);
                    cameraCloudFrameMsg.header.stamp    = ros::Time::now();
                    cameraCloudFrameMsg.header.frame_id = "/camera0_link";
                    pubCloudJ.publish(cameraCloudFrameMsg);

                    do {
                        cout << '\n' << "Press a key to continue...";
                    } while (std::cin.get() != '\n');
                }
            }
        }
    }

    myfile.close();

    std::cout << "added " << newEdgeCount << " edges" << std::endl;

    return 0;
}
