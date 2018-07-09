//
// Created by waxz on 18-7-4.
//

#include "locate_reflection/boardFinder.h"
#include <locate_reflection/patternMatcher.h>
#include <opencv2/opencv.hpp>
#include <GRANSAC/LineFitting.h>
#include <cpp_utils/svdlinefitting.h>

#define debug_pub true
#define debug_bypass true
BoardFinder::BoardFinder(ros::NodeHandle nh, ros::NodeHandle nh_private) :
        nh_(nh), nh_private_(nh_private), l(nh, nh_private) {

    scan_topic_ = "scan";
    odomtf_topic_ = "amcl_tf";

    baseLaserTf_.setIdentity();
    mapOdomTf_.setIdentity();


    // create scan sub
    auto res = l.createSubcriber<sensor_msgs::LaserScan>(scan_topic_, 10);

    // shared scan data
    laser_data_ = std::get<0>(res);


    // create tf sub
    auto res2 = l.createSubcriber<geometry_msgs::Pose>(odomtf_topic_, 1);

    // shared tf data
    mapOdom_data_ = std::get<0>(res2);
    if (!getMapOdomTf()) {
        ROS_ERROR("not received %s;exit", odomtf_topic_.c_str());
        exit(0);
    }

    // read xml
    string filename = "board.yaml";
    try {
        param_ = Yaml::readFile(filename);

    } catch (...) {
        printf("read %s failed!!\n", filename.c_str());
        exit(0);
    }




    // publish point  for rviz

    boardPub = nh_.advertise<geometry_msgs::PoseArray>("detectboard", 2);
    pointPub = nh_.advertise<geometry_msgs::PoseArray>("lightpoint", 2);
    markerPub = nh_.advertise<geometry_msgs::PoseArray>("board", 2);


    // point pattern matcher
    patternMatcher = PatternMatcher();

//


    // threading
//    threadClass_ = threading_util::ThreadClass();
    tfb_ = new tf::TransformBroadcaster();
    tfThread_.set(tfb_);

    // create shared data
    ros::Duration transform_tolerance;
    transform_tolerance.fromSec(0.1);

    tf::Transform transform;
    transform.setIdentity();
    ros::Time tn = ros::Time::now();
    ros::Time transform_expiration = (tn +
                                      transform_tolerance);

//            ROS_INFO("update odom by threads");
    tf::StampedTransform transformstamped(transform,
                                          transform_expiration,
                                          "map", "odom");



    mapToodomtfPtr_ = std::make_shared<tf::StampedTransform>();

    std::swap(*mapToodomtfPtr_, transformstamped);
#if 0
    threadClass_.setTarget(tfThread_, mapToodomtfPtr_);
#endif
    cout << " count = " << mapToodomtfPtr_.use_count() << endl;

    // start running in any function
//    threadClass_.start();





}

BoardFinder::~BoardFinder() {
    delete tfb_;
}

// get scan data
// if no data , return false;
bool BoardFinder::updateSensor() {
    bool getmsg = l.getOneMessage(scan_topic_, 0.1);
    size_t size = laser_data_.get()->ranges.size();
    if (bear_.size() != size) {
        bear_ = valarray<float>(0.0, size);
        for (int i = 0; i < size; i++) {
            bear_[i] = laser_data_.get()->angle_min + i * laser_data_.get()->angle_increment;
        }
    }
    return getmsg;
}

// get tf data
// if no data return false;
bool BoardFinder::getMapOdomTf() {
    // wait amcl tf from the first
    // at reboot, this tf only publish once
    bool getmsg = l.getOneMessage(odomtf_topic_, 3);
    if (getmsg)
        tf::poseMsgToTF(*mapOdom_data_, mapOdomTf_);
    else {


    }


    // todo:baypass
#if debug_bypass
    mapOdomTf_.setOrigin(tf::Vector3(-17.95, 10.8, 0.0));
    mapOdomTf_.setRotation(tf::Quaternion(0.0, 0.0, -0.144, 0.989));


    updateSharedData(mapOdomTf_);
    threadClass_.start();

    getmsg = true;
#endif
    return getmsg;


}

bool BoardFinder::getLaserPose() {

    ROS_INFO("getLaserPose start tf");
    if (!getMapOdomTf()) {
        ROS_INFO("no amcl tf ;skip");
        return false;
    }
//    l.getOneMessage("scan",-1);
    tf::Transform transform;

    transform.setIdentity();

    bool succ = l.getTransform("odom", "laser", transform, laser_data_.get()->header.stamp, 0.01, false);
    if (succ) {
        tf::poseTFToMsg(mapOdomTf_ * transform, laserPose_);
        ROS_INFO_STREAM("laserpose\n " << laserPose_);
    }

    return succ;
}

bool BoardFinder::getBoardPosition(vector<Position> &pointsW, vector<Position> &points) {

    if (!updateSensor()) {

        ROS_INFO("No laser, skip!!");
        return false;
    }
    if (!getLaserPose()) {
        ROS_INFO("No tf , skip!!");
        return false;
    }
    xmlToPoints(pointsW);
    points = pointsW;
    transformPoints(points);

    return true;

//    xmlToPoints();

}

// read position from xml file
// sort order in clockwise order; do this in transorm function
bool BoardFinder::xmlToPoints(vector<Position> &pointsW) {
    auto visibel_angle = param_["visibel_angle"].As<double>();
    auto visibel_range_ratio = param_["visibel_range_ratio"].As<double>();


    pointsW.clear();

    Position p;


//    return positions;

#if debug_pub

    geometry_msgs::PoseArray msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = "map";
#endif

    ROS_ERROR("get yaml board Num:%d", int(param_["board_position"].Size()));

    for (int it = 0; it < param_["board_position"].Size(); it++) {

        p.x = param_["board_position"][it]["x"].As<double>();

        p.y = param_["board_position"][it]["y"].As<double>();

        p.yaw = param_["board_position"][it]["yaw"].As<double>();

        p.length = param_["board_position"][it]["length"].As<double>();



        // check visibility
        double yaw = tf::getYaw(laserPose_.orientation);
        ROS_INFO("board %.3f,%.3f,%.3f,%.3f,%.3f,%.3f", p.x, p.y, p.yaw, laserPose_.position.x, laserPose_.position.y,
                 yaw);

        double distance = sqrt(pow(laserPose_.position.x - p.x, 2) +
                               pow(laserPose_.position.y - p.y, 2));
        if (distance > visibel_range_ratio * laser_data_.get()->range_max && distance < 0.1)
            continue;

        double boardtorobotangle = atan2(laserPose_.position.y - p.y, laserPose_.position.x - p.x);
        double robottoboardangle = atan2(p.y - laserPose_.position.y, p.x - laserPose_.position.x);
        p.angle = robottoboardangle;

//        bool in_view = robottoboardangle > laser_data_.get()->angle_min && robottoboardangle < laser_data_.get()->angle_max;
        double direction1 = normalDiff(boardtorobotangle, p.yaw);
        double direction2 = normalDiff(robottoboardangle, yaw);
        bool condition2 = direction2 > laser_data_.get()->angle_min && direction2 < laser_data_.get()->angle_max;
        bool condition1 = direction1 < visibel_angle;

        ROS_INFO("data inrange,%.3f,%.3f,%.3f,%.3f", robottoboardangle, boardtorobotangle, direction1, direction2);
        ROS_INFO("limit: visibel_angle=%.3f,angle_min=%.3f,angle_max=%.3f", visibel_angle, laser_data_.get()->angle_min,
                 laser_data_.get()->angle_max);


        if (condition1 && condition2) {
            pointsW.push_back(p);

#if debug_pub

            geometry_msgs::Point point;
            point.x = p.x;
            point.y = p.y;

            geometry_msgs::Pose pose;
            tf::poseTFToMsg(tf_util::createTransformFromTranslationYaw(point, p.yaw), pose);
            msg.poses.push_back(pose);
#endif

        }

    }
    ROS_INFO("get boarder:%d", int(pointsW.size()));
#if debug_pub


    markerPub.publish(msg);
#endif


    // sort points
    std::sort(pointsW.begin(), pointsW.end(), angleCompare);

    return true;

}


vector<Position> BoardFinder::detectBoard() {
    // upddate sensor
    vector<Position> ps;

    ROS_INFO("start scan");
    if (!updateSensor())
        return ps;

    // vector to valarray
    valarray<float> ranges = container::createValarrayFromVector<float>(laser_data_.get()->ranges);
    valarray<float> intensities = container::createValarrayFromVector<float>(laser_data_.get()->intensities);
    if (intensities.size() == 0) {
        ROS_ERROR("scan has no intensities ");
        return ps;
        exit(0);
    }

    valarray<float> xs = ranges * cos(bear_);
    valarray<float> ys = ranges * sin(bear_);


    auto intensity_thresh = param_["intensity_thresh"].As<float>();
    auto neighbor_thresh = param_["neighbor_thresh"].As<float>();
    auto length_thresh = param_["length_thresh"].As<float>();

    // point neighbor threash = num*bean_angle*distance;
    neighbor_thresh = neighbor_thresh * laser_data_.get()->angle_increment;

//    valarray<float> lightPoitns = intensities[intensities>intensity_thresh];
    valarray<float> lightXs = xs[intensities > intensity_thresh];
    valarray<float> lightYs = ys[intensities > intensity_thresh];
    valarray<float> rangesMask = ranges[intensities > intensity_thresh];

    size_t size = lightXs.size();
    if (size == 0) {

        ROS_ERROR("no light point");
        return ps;

    }

    valarray<float> lightXsL = lightXs[std::slice(0, size - 1, 1)];
    valarray<float> lightXsR = lightXs[std::slice(1, size - 1, 1)];

    valarray<float> lightYsL = lightYs[std::slice(0, size - 1, 1)];
    valarray<float> lightYsR = lightYs[std::slice(1, size - 1, 1)];

    valarray<float> distance = sqrt(pow(lightXsR - lightXsL, 2) + pow(lightYsR - lightYsL, 2));


//    for (int i =0;i< distance.size();i++)
//        cout<<distance[i]<<",";


    Position p;
    size_t pointNum = 0;
    double pointLength = 0;

    //publish pose
    geometry_msgs::PoseArray msg, lightpoints;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = "laser";
    lightpoints.header.stamp = ros::Time::now();
    lightpoints.header.frame_id = "laser";
    geometry_msgs::Pose pose, lightpose;


    for (int i = 0; i < distance.size(); i++) {
        double thresh = neighbor_thresh * rangesMask[i];
//        thresh = 0.5;

        double d = distance[i];
        if (d < thresh) {

            pointNum++;
        }
        if (d > thresh || (i == distance.size() - 1 && d < thresh)) {

            if (i == distance.size() - 1) {
                i++;
            }
            double length = sqrt(
                    pow(lightXs[i] - lightXs[i - pointNum], 2) + pow(lightYs[i] - lightYs[i - pointNum], 2));
            //compute length

            if (length > length_thresh) {

                // get index
                vector<size_t> idx_vec;
                // push light point to vector
                // fit line
                vector<cv::Point2d> FitPoints;
                for (size_t it = i - pointNum; it < i + 1; it++) {
                    idx_vec.push_back(it);
                    lightpose.position.x = lightXs[it];
                    lightpose.position.y = lightYs[it];
                    lightpoints.poses.push_back(lightpose);
                    FitPoints.push_back(cv::Point2f(lightXs[it], lightYs[it]));

                }
                //call rnasac fit
                //double angle = LineFitting(FitPoints);

                // call svd line fit
                double angle = svdfit(FitPoints);



                valarray<size_t> idx_val = container::createValarrayFromVector<size_t>(idx_vec);

                valarray<float> cluster_x = lightXs[idx_val];

                double center_x = cluster_x.sum() / cluster_x.size();

                valarray<float> cluster_y = lightYs[idx_val];

                double center_y = cluster_y.sum() / cluster_y.size();

                // angle shoud point to robot
                double angletorobot = atan2(-center_y, -center_x);

                double minangle = 20;
                double tmpangle = angle;

                for (int c = 0; c < 2; c++) {
                    cout << angle;
                    if (normalDiff(angletorobot, (angle + ((c > 0) ? 1 : -1) * 0.5 * M_PI)) < minangle) {
                        tmpangle = angle + ((c > 0) ? 1 : -1) * 0.5 * M_PI;
                        minangle = normalDiff(angletorobot, tmpangle);

                    }
                }
                angle = tmpangle;
                p.x = center_x;
                p.y = center_y;
                p.yaw = angle;

                ps.push_back(p);
                cout << "x" << p.x << "y" << p.y << "angle" << p.yaw << "length:" << length << std::endl;


                pose.position.x = p.x;
                pose.position.y = p.y;
                pose.orientation = tf::createQuaternionMsgFromYaw(angle);
                msg.poses.push_back(pose);
            }

            pointNum = 0;

        };

    }

    cout << "get board num " << ps.size();
    ROS_INFO("finish scan");

#if debug_pub
    boardPub.publish(msg);
    pointPub.publish(lightpoints);
#endif
    return ps;
}

bool BoardFinder::transformPoints(vector<Position> &realPoints) {
    // get odom to laser transform
    tf::Transform mapLasertf;
    tf::poseMsgToTF(laserPose_, mapLasertf);


    geometry_msgs::Point point;
#if debug_pub

    geometry_msgs::PoseArray msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = "laser";
#endif

    for (int i = 0; i < realPoints.size(); i++) {

        tf::Point p(realPoints[i].x, realPoints[i].y, 0.0);
        tf::pointTFToMsg(mapLasertf.inverse() * p, point);

        realPoints[i].y = point.y;
        realPoints[i].x = point.x;

        geometry_msgs::Pose pose;
        tf::poseTFToMsg(tf_util::createTransformFromTranslationYaw(point, 0.0), pose);
        msg.poses.push_back(pose);
    }

    // sort

    markerPub.publish(msg);

    return true;
}


bool
BoardFinder::findNN(vector<Position> &realPointsW, vector<Position> &realPoints, vector<Position> &detectPoints) {


    vector<Position> realPointReg, detecctPointReg;

    // call pattern match
    // how to detect failure
    auto assign = patternMatcher.match(detectPoints, realPoints);
    if (assign.empty()) {

        cout << "match failure!!" << endl;
        return false;

    }

    for (int i = 0; i < assign.size(); i++) {
        realPointReg.push_back(realPointsW[std::get<1>(assign[i])]);
        detecctPointReg.push_back(detectPoints[std::get<0>(assign[i])]);
    }
    realPointsW = realPointReg;
    detectPoints = detecctPointReg;

    return true;

#if 0
    realPointReg = realPoints;
    bool zerofisrt = false;
    if (realPoints[0].y > realPoints[1].y) {
        zerofisrt = true;
    }
    Position tmp;

    if ((detectPointsW[0].y > detectPointsW[1].y) != (realPoints[0].y > realPoints[1].y)) {
        realPoints[0] = realPointReg[1];
        realPoints[1] = realPointReg[0];

    }

    return;

#endif

#if 0

    // create kdtree
    const int npoints = realPoints.size();
    std::vector<kdtree::Point2d> points(npoints);
    for (int i = 0; i < npoints; i++) {
        points[0] = kdtree::Point2d(realPoints[i].x, realPoints[i].y);
    }


    // build k-d tree
    kdtree::KdTree<kdtree::Point2d> kdtree(points);

    double radius = 2.0;
    // query point
    vector<vector<int> > results;
    for (int i = 0; i < detectPointsW.size(); i++) {
        // create query point
        //search
        vector<int> res;


        kdtree::Point2d query(detectPointsW[i].x, detectPointsW[i].y);
        res = kdtree.queryIndex(query, kdtree::SearchMode::radius, radius);

        results.push_back(res);
    }

    int startidx = 0;
    for (int i = 0; i < results.size(); i++) {
        if (results[i].size() == 1) {

            detecctPointReg.push_back(detectPoints[i]);
            realPointReg.push_back(realPoints[results[i][0]]);
        }
    }

    realPoints = realPointReg;
    detectPoints = detecctPointReg;

    // check results, remove duplicate or invalid points;


    // start with a point matcn only one point
    // maybe it's fake detection


    // clear and check distancce  and sort vector
#endif
}

void BoardFinder::updateMapOdomTf(tf::Transform laserPose, ros::Time time) {

    tf::Transform odomTobase;

    if (!l.getTransform("odom", "base_link", odomTobase, time))
        return;

    if (baseLaserTf_.getOrigin().getX() == 0.0)
        l.getTransform("base_link", "laser", baseLaserTf_);

    mapOdomTf_ = laserPose * baseLaserTf_.inverse() * odomTobase.inverse();

    cout << " count = " << mapToodomtfPtr_.use_count() << endl;

    updateSharedData(mapOdomTf_);



//    l.sendTransform("map", "odom", mapTOodomTf, 0.1);


}

void BoardFinder::updateSharedData(tf::Transform mapTOodomTf) {

    ros::Time tn = ros::Time::now();
    ros::Duration transform_tolerance;
    transform_tolerance.fromSec(0.1);
    ros::Time transform_expiration = (tn + transform_tolerance);



    // todo:segementation  fault???
    // empy ? count 0
    mapToodomtfPtr_.get()->setOrigin(mapOdomTf_.getOrigin());
    mapToodomtfPtr_.get()->setRotation(mapOdomTf_.getRotation());
    mapToodomtfPtr_.get()->stamp_ = transform_expiration;
//    std::swap(*mapToodomtfPtr_, mapTOodomTfstamped);

    cout << "23333" << endl;

}

void BoardFinder::computeUpdatedPose(vector<Position> realPoints, vector<Position> detectPoints) {


    geometry_msgs::Point translation;
    double realX = 0, realY = 0, detectX = 0, detectY = 0;
    double realyaw, detectyaw;
    tf::Transform realTarget, detectTarget, laserPose;


    if (detectPoints.size() > 1) {
        // get points pairs
        // compute target vector
        int size = realPoints.size();
        for (int i = 0; i < size; i++) {
            realX += realPoints[i].x;
            realY += realPoints[i].y;
            detectX += detectPoints[i].x;
            detectY += detectPoints[i].y;

        }
        realX /= size;
        realY /= size;
        detectX /= size;
        detectY /= size;


        realyaw = std::atan2(realPoints[0].y - realPoints[size - 1].y, realPoints[0].x - realPoints[size - 1].x);
        detectyaw = std::atan2(detectPoints[0].y - detectPoints[size - 1].y,
                               detectPoints[0].x - detectPoints[size - 1].x);
    } else {
        realX = realPoints[0].x;
        realY = realPoints[0].y;
        realyaw = realPoints[0].yaw;
        detectX = detectPoints[0].x;
        detectY = detectPoints[0].y;
        detectyaw = detectPoints[0].yaw;

    }

    translation.x = realX;
    translation.y = realY;

    realTarget = tf_util::createTransformFromTranslationYaw(translation, realyaw);
    translation.x = detectX;
    translation.y = detectY;
    detectTarget = tf_util::createTransformFromTranslationYaw(translation, detectyaw);


    //compute laser pose

    laserPose = realTarget * detectTarget.inverse();





    // compute base_link pose


    // get odom-baselink

    updateMapOdomTf(laserPose, laser_data_.get()->header.stamp);


    // compute map-odom


}


// find reflection board and update map-odom tf
// get board in map ; given baselink
// get board from scan
// match two point pattern
// compute relative pose
// update tf
void BoardFinder::findLocation() {
    // get real board position
    vector<Position> realPointsW;
    vector<Position> realPoints;
    getBoardPosition(realPointsW, realPoints);


    // detect board position
    vector<Position> detectPoints = detectBoard();
//    vector<Position> detectPointsW = detectPoints;



#if 0
    // test matcher;ok
    vector<Position> p1, p2;
    p1.push_back(Position(-1, 2, 3.3,1));
    p1.push_back(Position(1, 2, 3.3,1));
    p1.push_back(Position(1.5, 1.5, 3.3,1));

    p2.push_back(Position(-3.1, 1.9, 3.3,1));
    p2.push_back(Position(-1.1, 1.9, 3.3,1));
    p2.push_back(Position(0.9, 1.9, 3.3,1));
    p2.push_back(Position(1.4, 1.4, 3.3,1));
    realPoints = p2;
    realPointsW = p2;
    detectPoints = p1;
#endif


    // if successful
    if (realPoints.size() > 1 && detectPoints.size() > 1) {

        // transform detectPoints to map frame





        // find match and sort vector
        // convert map board to laser frame

        //todo:bypass

        findNN(realPointsW, realPoints, detectPoints);

        // if detect more board
        // 1 board: compute with position and yaw
        // 2 and more , compute with relative position of betwwen board

        if (!detectPoints.empty()) {
            // compute target vector
            computeUpdatedPose(realPointsW, detectPoints);

        }


#if 0
        computeTagetVector();
        getBasePose();
        updateOdom();
#endif

    }
}



