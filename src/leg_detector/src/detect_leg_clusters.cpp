/*********************************************************************
* Software License Agreement (BSD License)
* 
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
* 
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
* 
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
* 
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

// ROS related Headers
#include <rclcpp/rclcpp.hpp>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/create_timer_interface.h>
#include <tf2_ros/create_timer_ros.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2/transform_datatypes.h>
#include <visualization_msgs/msg/marker.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <memory>

// OpenCV related Headers
#include <opencv2/core/core.hpp>
#include <opencv2/ml/ml.hpp>
#include <opencv2/ml.hpp>

// Local Headers
#include "leg_detector/cluster_features.h"
#include "leg_detector/laser_processor.h"

// Custom Messages related Headers
#include "leg_detector_msgs/msg/leg.hpp"
#include "leg_detector_msgs/msg/leg_array.hpp"

#include <fstream>

class DetectLegClusters : public rclcpp::Node
{
public:
    DetectLegClusters() : Node("detect_leg_clusters")
    {

        //Get ROS parameters
        std::string forest_file;
        std::string scan_topic;
        num_prev_markers_published_ = 0;
        scan_num_ = 0;

        scan_topic = this->declare_parameter<std::string>("scan_topic", "/scan"); 
        fixed_frame_ = this->declare_parameter<std::string>("fixed_frame", "laser");
        detection_threshold_ = this->declare_parameter<double>("detection_threshold", -1.0);
        cluster_dist_euclid_ = this->declare_parameter<double>("cluster_dist_euclid", 0.13);
        min_points_per_cluster_ = this->declare_parameter<int>("min_points_per_cluster", 3);
        max_detect_distance_ = this->declare_parameter<double>("max_detect_distance", 10.0);
        max_detected_clusters_ = this->declare_parameter<int>("max_detected_clusters", -1);
        use_scan_header_stamp_for_tfs_ = this->declare_parameter<bool>("use_scan_header_stamp_for_tfs", false);
        forest_file = this->declare_parameter<std::string>("forest_file", "/root/ros2_ws/src/leg_tracker_ros2/config/trained_leg_detector_res=0.33.yaml");

        //Print the ROS parameters
        RCLCPP_INFO(this->get_logger(), "forest_file: %s", forest_file.c_str());
        RCLCPP_INFO(this->get_logger(), "scan_topic: %s", scan_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "fixed_frame: %s", fixed_frame_.c_str());
        RCLCPP_INFO(this->get_logger(), "detection_threshold: %.2f", detection_threshold_);
        RCLCPP_INFO(this->get_logger(), "cluster_dist_euclid: %.2f", cluster_dist_euclid_);
        RCLCPP_INFO(this->get_logger(), "min_points_per_cluster: %d", min_points_per_cluster_);
        RCLCPP_INFO(this->get_logger(), "max_detect_distance: %.2f", max_detect_distance_);
        RCLCPP_INFO(this->get_logger(), "marker_display_lifetime: %.2f", marker_display_lifetime_);
        RCLCPP_INFO(this->get_logger(), "use_scan_header_stamp_for_tfs: %d", use_scan_header_stamp_for_tfs_);
        RCLCPP_INFO(this->get_logger(), "max_detected_clusters: %d", max_detected_clusters_);

        //Load Random forest
        forest = cv::ml::StatModel::load<cv::ml::RTrees>(forest_file);
        feat_count_ = forest->getVarCount();

        latest_scan_header_stamp_with_tf_available_ = this->now();
        auto default_qos = rclcpp::QoS(rclcpp::SystemDefaultsQoS());

        /**Define the publishers and subscribers
              * This node will publish 2 topics
              * 1. visualization_marker  : message type - <visualization_msgs::Marker>
              * 2. detected_leg_clusters : message type - <leg_detector_msgs::msg::LegArray>
              * This node will subscribe to 1 topic
              * 1. scan_topic
            ***/
        markers_pub_ = this->create_publisher<visualization_msgs::msg::Marker>("visualization_marker", 20);
        detected_leg_clusters_pub_ = this->create_publisher<leg_detector_msgs::msg::LegArray>("detected_leg_clusters", 20);
        this->scan_sub_ = this->create_subscription<sensor_msgs::msg::LaserScan>(scan_topic, default_qos, std::bind(&DetectLegClusters::laserCallback, this, std::placeholders::_1));

        buffer_ = std::make_unique<tf2_ros::Buffer>(this->get_clock());
        tfl_ = std::make_shared<tf2_ros::TransformListener>(*buffer_);
        auto timer_interface = std::make_shared<tf2_ros::CreateTimerROS>(
                                                            this->get_node_base_interface(),
                                                            this->get_node_timers_interface());
        buffer_->setCreateTimerInterface(timer_interface);
    }

private:

    std::shared_ptr<tf2_ros::Buffer> buffer_;
    std::shared_ptr<tf2_ros::TransformListener> tfl_;
    
            
    cv::Ptr<cv::ml::RTrees> forest = cv::ml::RTrees::create();

    int feat_count_;

    ClusterFeatures cf_;
    
    int scan_num_;
    int num_prev_markers_published_;
    bool use_scan_header_stamp_for_tfs_;

    rclcpp::Time latest_scan_header_stamp_with_tf_available_;

    std::string fixed_frame_;

    double detection_threshold_;
    double cluster_dist_euclid_;
    int min_points_per_cluster_;
    double max_detect_distance_;
    double marker_display_lifetime_;
    int max_detected_clusters_;

    //create the publisher and subscribers

    rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr markers_pub_;
    rclcpp::Publisher<leg_detector_msgs::msg::LegArray>::SharedPtr detected_leg_clusters_pub_;
    rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr scan_sub_;

    /**
     * @brief Clusters the scan according to euclidian distance, 
     *        predicts the confidence that each cluster is a human leg and publishes the results
     * 
     * Called every time a laser scan is published.
     */
    void laserCallback(const sensor_msgs::msg::LaserScan::SharedPtr scan)
    {
        laser_processor::ScanProcessor processor(*scan);
        processor.splitConnected(cluster_dist_euclid_);
        processor.removeLessThan(min_points_per_cluster_);
        
        // OpenCV matrix needed to use the OpenCV random forest classifier
        // CvMat* tmp_mat = cvCreateMat(1, feat_count_, CV_32FC1);
        cv::Mat tmp_mat(1, feat_count_, CV_32FC1);

        leg_detector_msgs::msg::LegArray detected_leg_clusters;
        detected_leg_clusters.header.frame_id = scan->header.frame_id;
        detected_leg_clusters.header.stamp = scan->header.stamp;

        // Find out the time that should be used for tfs
        bool transform_available;
        rclcpp::Clock tf_time;
        rclcpp::Time tf_time1;
        
        // Use time from scan header
        if (use_scan_header_stamp_for_tfs_) 
        {
            tf_time1 = scan->header.stamp;

            try {
                buffer_->lookupTransform(fixed_frame_, scan->header.frame_id, tf_time1, rclcpp::Duration(1, 0));
                transform_available = buffer_->canTransform(fixed_frame_, scan->header.frame_id, tf_time1);              
            } catch(tf2::TransformException &e) {
                RCLCPP_INFO (this->get_logger(), "Stopped here : Detect_leg_clusters: No tf available");
                transform_available = false;
                
            }
        } else {

            // Otherwise just use the latest tf available
            
            tf_time.now();
            transform_available = buffer_->canTransform(fixed_frame_, scan->header.frame_id, tf_time1);
        }

        // Store all processes legs in a set ordered according to their relative distance to the laser scanner
        std::set<leg_detector_msgs::msg::Leg, CompareLegs> leg_set;

        if(!transform_available) {
            RCLCPP_INFO (this->get_logger(), "Stop point 2 : dNot publishing detected leg clusters because no tf was available");
        } else {

            // Iterate through all clusters
            for (std::list<laser_processor::SampleSet*>::iterator cluster = processor.getClusters().begin();cluster != processor.getClusters().end(); cluster++)
            {
                // Get position of cluster in laser frame
                std::string frame_id = scan->header.frame_id;                               
                geometry_msgs::msg::PointStamped position;
                geometry_msgs::msg::PointStamped position1;
                position.header.frame_id = frame_id;
                position.header.stamp = scan->header.stamp;
                position.point = (*cluster)->getPosition();
                float rel_dist = pow(position.point.x*position.point.x + position.point.y*position.point.y, 1./2.);

                // Only consider clusters within max_distance
                if (rel_dist < max_detect_distance_) {

                    // Classify cluster using random forest classifier
                    std::vector<float> f = cf_.calcClusterFeatures(*cluster, *scan);
                    cv::Mat tmp_mat = cv::Mat(1, feat_count_, CV_32F, f.data()); // 特徴量ベクトルをcv::Matに変換 

                    // The forest->predict funciton has been removed in the latest versions of OpenCV so we'll do the calculation explicitly.
                    cv::Mat result;
                    forest->getVotes(tmp_mat, result, 0); // Direct use of cv::Mat
                    int positive_votes = result.at<int>(1, 1);
                    int negative_votes = result.at<int>(1, 0);
                    float probability_of_leg = positive_votes / static_cast<double>(positive_votes + negative_votes);
                    // RCLCPP_INFO (this->get_logger(), "Checkout 6");

                    // Consider only clusters that have a confidence greater than detection_threshold_
                    if (probability_of_leg > detection_threshold_)
                    {
                        // Transform cluster position to fixed frame
                        // This should always be succesful because we've checked earlier if a tf was available
                        bool transform_successful_2;
                        try {
                            buffer_->transform(position, position, fixed_frame_);
                            transform_successful_2 = true;
                        } catch (tf2::TransformException &e){
                            RCLCPP_ERROR (this->get_logger(), "%s", e.what());
                            transform_successful_2 = false;
                        }

                        if (transform_successful_2) {
                            // Add detected cluster to set of detected leg clusters, along with its relative position to the laser scanner
                            leg_detector_msgs::msg::Leg new_leg;
                            new_leg.position.x = position.point.x;
                            new_leg.position.y = position.point.y;
                            new_leg.confidence = probability_of_leg;
                            leg_set.insert(new_leg);
                        }
                    // else 
                    // {
                    //     RCLCPP_INFO(this->get_logger(), "skipped cluster");
                    // }
                    }
                }
            }
        }

        // Publish detected legs to /detected_leg_clusters and to rviz
        // They are ordered from closest to the laser scanner to furthest
        int clusters_published_counter = 0;
        int id_num = 1;

        for (std::set<leg_detector_msgs::msg::Leg>::iterator it = leg_set.begin(); it != leg_set.end(); ++it){

            // Publish to /detected_leg_clusters topic
            leg_detector_msgs::msg::Leg leg = *it;
            detected_leg_clusters.legs.push_back(leg);
            clusters_published_counter++;
            visualization_msgs::msg::Marker m;
            m.header.stamp = scan->header.stamp;
            m.header.frame_id = fixed_frame_;
            m.ns = "LEGS";
            m.id = id_num++;
            m.type = m.SPHERE;
            m.pose.position.x = leg.position.x;
            m.pose.position.y = leg.position.y;
            m.pose.position.z = 0.2;
            m.scale.x = 0.13;
            m.scale.y = 0.13;
            m.scale.z = 0.13;
            m.color.a = 1;
            m.color.r = leg.confidence;
            m.color.g = leg.confidence;
            m.color.b = leg.confidence;
            markers_pub_->publish(m);

            // Comparison using '==' and not '>=' is important, as it allows <max_detected_clusters_>=-1 
            // to publish infinite markers
            if (clusters_published_counter == max_detected_clusters_) 
                break;
        }
        //debug_file.close();

        // Clear remaining markers in Rviz
        for (int id_num_diff = num_prev_markers_published_-id_num; id_num_diff > 0; id_num_diff--) 
        {           
            visualization_msgs::msg::Marker m;
            m.header.stamp = scan->header.stamp;
            m.header.frame_id = fixed_frame_;
            m.ns = "LEGS";
            m.id = id_num_diff + id_num;
            m.color.a = 0;
            m.action = m.DELETE;
            markers_pub_->publish(m);

        }
        num_prev_markers_published_ = id_num; // For the next callback
        RCLCPP_INFO(this->get_logger(), "clusters size: %d", detected_leg_clusters.legs.size());
        detected_leg_clusters_pub_->publish(detected_leg_clusters);
        // cvReleaseMat(&tmp_mat);
    }

    /**
         * @brief Comparison class to order Legs according to their relative distance to the laser scanner
        **/
    class CompareLegs
    {
    public:
        bool operator()(const leg_detector_msgs::msg::Leg &a, const leg_detector_msgs::msg::Leg &b) const {

            float rel_dist_a = pow(a.position.x * a.position.x + a.position.y * a.position.y, 1. / 2.);
            float rel_dist_b = pow(b.position.x * b.position.x + b.position.y * b.position.y, 1. / 2.);
            return rel_dist_a < rel_dist_b;
        }
    };
};

int main(int argc, char **argv)
{

    rclcpp::init(argc, argv);
    auto node = std::make_shared<DetectLegClusters>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}