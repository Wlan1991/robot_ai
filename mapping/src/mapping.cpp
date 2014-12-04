#include "mapping/mapping.h"

const int Mapping::GRID_HEIGHT = 1000;
const int Mapping::GRID_WIDTH = 1000;

const double Mapping::MAP_HEIGHT = 10.0;
const double Mapping::MAP_WIDTH = 10.0;
const double Mapping::MAP_X_OFFSET = MAP_WIDTH/2.0;
const double Mapping::MAP_Y_OFFSET = MAP_HEIGHT/2.0;

const double Mapping::P_PRIOR = log(0.5);
const double Mapping::P_OCC = log(0.9); //log(0.7);
const double Mapping::P_FREE  = log(0.4);//log(0.35);

const double Mapping::FREE_OCCUPIED_THRESHOLD = log(0.5);

const double Mapping::MAX_IR_DIST = 0.5;
const double Mapping::MIN_IR_DIST = 0.04;

const double Mapping::INVALID_READING = -1.0;
const int Mapping::UNKNOWN = 50;
const int Mapping::FREE = 0;
const int Mapping::OCCUPIED = 100;
const int Mapping::BLUE_CUBE = 2;
const int Mapping::RED_SPHERE = 3;

typedef pcl::PointCloud<pcl::PointXYZI> PointCloud;
typedef pcl::PointXYZI PCPoint;

Mapping::Mapping() :
    fl_ir_reading(INVALID_READING), fr_ir_reading(INVALID_READING),
    bl_ir_reading(INVALID_READING), br_ir_reading(INVALID_READING),
    pos(Point<double>(0.0,0.0)), turning(false),
    wall_planes(new std::vector<common::vision::SegmentedPlane>()),
    markers("map","raycasts")
{
    handle = ros::NodeHandle("");
    distance_sub = handle.subscribe("/perception/ir/distance", 1, &Mapping::distanceCallback, this);
    odometry_sub = handle.subscribe("/pose/odometry/", 1, &Mapping::odometryCallback, this);
    start_turn_sub = handle.subscribe("/controller/turn/angle", 1, &Mapping::startTurnCallback, this);
    stop_turn_sub = handle.subscribe("/controller/turn/done", 1, &Mapping::stopTurnCallback, this);
     wall_sub = handle.subscribe("/vision/obstacles/planes", 1, &Mapping::wallDetectedCallback, this);
   // object_sub = handle.subscribe("/vision/object/position", 1, &Mapping::objectDetectedCallback, this);
    map_pub = handle.advertise<nav_msgs::OccupancyGrid>("/mapping/occupancy_grid", 1);
    pub_viz = handle.advertise<visualization_msgs::MarkerArray>("visualization_marker_array",10);
    
    srv_raycast = handle.advertiseService("/mapping/raycast", &Mapping::performRaycast, this);

    occupancy_grid.header.frame_id = "world";
    nav_msgs::MapMetaData metaData;
    metaData.resolution = 0.01;
    metaData.width = GRID_WIDTH;
    metaData.height = GRID_HEIGHT;
    metaData.origin.position.x = -MAP_X_OFFSET;
    metaData.origin.position.y = -MAP_Y_OFFSET;
    metaData.origin.orientation.x = 180.0;
    metaData.origin.orientation.y = 180.0;
 //   metaData.origin.orientation.y = 180.0;
   // metaData.origin.orientation.w = 180.0;
    occupancy_grid.info = metaData;
   
    initProbabilityGrid();
    initOccupancyGrid();
}

void Mapping::updateGrid()
{
    // if(turning)
    //     return;

    updateIR(fl_ir_reading, robot::ir::offset_front_left_forward);
    updateIR(-fr_ir_reading, robot::ir::offset_front_right_forward);
    updateIR(-br_ir_reading, -robot::ir::offset_rear_right_forward);
    updateIR(bl_ir_reading, -robot::ir::offset_rear_left_forward);
}

void Mapping::markPointsFreeBetween(Point<double> p1, Point<double> p2)
{
    Point<double> min = p1.x <= p2.x ? p1 : p2;
    Point<double> max = p1.x > p2.x ? p1 : p2;
    double dx = max.x-min.x;
    double dy = max.y-min.y;

    if(dx < 0.01) {
        min = p1.y <= p2.y ? p1 : p2;
        max = p1.y > p2.y ? p1 : p2;
        double x = min.x;
        for(double y = min.y; y < max.y; y +=0.01)
        {
            markPointFree(Point<double>(x,y));
        }
    } else {
        for(double x = min.x; x < max.x; x+=0.01)
        {
            double y = min.y + dy * (x-min.x) / dx;
            markPointFree(Point<double>(x,y));
        }
    }
}

void Mapping::updateIR(double ir_reading, double ir_x_offset)
{
    Point<double> ir_pos = Point<double>(ir_x_offset, 0);
    if(isIRValid(ir_reading))
    {
        Point<double> obstacle(ir_x_offset, ir_reading);
        markPointOccupied(obstacle);
        markPointsFreeBetween(ir_pos, obstacle);
    } else {
        double mult = ir_reading > 0 ? 1.0 : -1.0;
        Point<double> max_point = Point<double>(ir_pos.x, MAX_IR_DIST*0.75*mult);
        markPointsFreeBetween(ir_pos, max_point);
    }
}

void Mapping::updateWalls()
{
    for(int i = 0; i < wall_planes->size(); ++i)
    {
        if(wall_planes->at(i).is_ground_plane())
            continue;

    }
}

void Mapping::markPointOccupied(Point<double> point)
{
    Point<int> cell = robotPointToCell(point);
    int x = cell.x;
    int y = cell.y;
    for(int i = x-1; i < x+1; ++i)
    {
        for(int j = y-1; j < y+1; ++j)
        {
            Point<int> c(i, j);
            markProbabilityGrid(c, P_OCC);
            updateOccupancyGrid(c);
        }
    }
}

void Mapping::markPointFree(Point<double> point)
{
    Point<int> cell = robotPointToCell(point);
    markProbabilityGrid(cell, P_FREE);
    updateOccupancyGrid(cell);
}

Point<int> Mapping::robotPointToCell(Point<double> point)
{
    Point<double> map_point = robotToMapTransform(point);
    return mapPointToCell(map_point);
}

Point<double> Mapping::robotToMapTransform(Point<double> point)
{
    geometry_msgs::PointStamped robot_point_msg;
    robot_point_msg.header.frame_id = "robot";
    robot_point_msg.header.stamp = ros::Time::now();
    robot_point_msg.point.x = point.x;
    robot_point_msg.point.y = point.y;
    robot_point_msg.point.z = 0.0;
    tf::Stamped<tf::Point> robot_point;
    tf::pointStampedMsgToTF(robot_point_msg, robot_point);
    tf::Vector3 map_point = transform*robot_point;

    Point<double> res = Point<double>(map_point.getX() + MAP_X_OFFSET, map_point.getY() + MAP_Y_OFFSET);
    return res;
}

Point<int> Mapping::mapPointToCell(Point<double> point)
{
    int x = round(point.x*100.0);
    int y = round(point.y*100.0);
    return Point<int>(x,y);
}

void Mapping::markProbabilityGrid(Point<int> cell, double log_prob)
{
    prob_grid[cell.y][cell.x] += log_prob - P_PRIOR;
}

void Mapping::updateOccupancyGrid(Point<int> cell)
{
    int pos = cell.x*GRID_WIDTH + cell.y;
    if(prob_grid[cell.y][cell.x] > FREE_OCCUPIED_THRESHOLD)
        occupancy_grid.data[pos] = OCCUPIED;
    else if(prob_grid[cell.y][cell.x] < FREE_OCCUPIED_THRESHOLD)
        occupancy_grid.data[pos] = FREE;
    else
        occupancy_grid.data[pos] = UNKNOWN;
}

bool Mapping::isIRValid(double value)
{
    return std::abs(value) < MAX_IR_DIST && std::abs(value) > MIN_IR_DIST;
}

void Mapping::initProbabilityGrid()
{
    prob_grid.resize(GRID_HEIGHT);
    for(int i = 0; i < GRID_HEIGHT; ++i)
        prob_grid[i].resize(GRID_WIDTH);

    for(int i = 0; i < GRID_HEIGHT; ++i)
        for(int j = 0; j < GRID_WIDTH; ++j)
            prob_grid[i][j] = P_PRIOR;
}

void Mapping::initOccupancyGrid()
{
    occupancy_grid.data.resize(GRID_WIDTH*GRID_HEIGHT);
    for(int i = 0; i < GRID_HEIGHT*GRID_WIDTH; ++i)
            occupancy_grid.data[i] = UNKNOWN;
}

void Mapping::distanceCallback(const ir_converter::Distance::ConstPtr& distance)
{   
    fl_ir_reading = distance->fl_side;
    bl_ir_reading = distance->bl_side;
    fr_ir_reading = distance->fr_side;
    br_ir_reading = distance->br_side;
}

void Mapping::odometryCallback(const nav_msgs::Odometry::ConstPtr& odom)
{
    double x = odom->pose.pose.position.x;
    double y = odom->pose.pose.position.y;
    pos = Point<double>(x,y);
}

void Mapping::startTurnCallback(const std_msgs::Float64::ConstPtr & angle)
{
    turning = true;
}

void Mapping::stopTurnCallback(const std_msgs::Bool::ConstPtr & var)
{
    turning = false;
}

void Mapping::updateTransform()
{
    try {
        tf_listener.waitForTransform("map", "robot", ros::Time(0), ros::Duration(10.0) );
        tf_listener.lookupTransform("map", "robot", ros::Time(0), transform);
    } catch (tf::TransformException ex) {
        ROS_ERROR("%s",ex.what());
    }
}

void Mapping::wallDetectedCallback(const vision_msgs::Planes::ConstPtr & msg)
{
    wall_planes->clear();
    common::vision::msgToPlanes(msg, wall_planes);

}

void Mapping::publishMap()
{
    map_pub.publish(occupancy_grid);
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "mapping");
    Mapping mapping;
    ros::Rate loop_rate(20);

    int counter = 0;
    while(ros::ok())
    {
        mapping.updateTransform();
        ++counter;
        mapping.updateGrid();
        if(counter % 10 == 0)
            mapping.publishMap();
        ros::spinOnce();
        loop_rate.sleep();
    }
}

bool Mapping::isObstacle(int x, int y)
{
    if (y < 0 || y >= prob_grid.size())
        return false;
    if (x < 0 || x >= prob_grid[y].size())
        return false;

    if(prob_grid[y][x] > FREE_OCCUPIED_THRESHOLD)
        return true;
}

void addPointToList(std::vector<Point<int> >& list, int x, int y)
{
    if (!list.empty()) {
        Point<int> p(x,y);
        Point<int>& last = list[list.size()-1];

        if (p.x != last.x || p.y != last.y)
            list.push_back(p);
    }
    else list.push_back(Point<int>(x,y));
}

bool Mapping::performRaycast(navigation_msgs::RaycastRequest &request, navigation_msgs::RaycastResponse &response)
{
    Eigen::Vector2f origin(request.origin_x, request.origin_y);
    Eigen::Vector2f dir(request.dir_x, request.dir_y);
    dir.normalize();
    dir *= request.max_length;

    Point<double> robot_p1(origin(0), origin(1));
    Point<double> robot_p2(origin(0) + dir(0), origin(1) + dir(1));

    Point<double> p1 = robotToMapTransform(robot_p1);
    Point<double> p2 = robotToMapTransform(robot_p2);

    std::vector<Point<int> > obstacle_points;

    //line draw algorithm again to find obstacles
    Point<double>& min = p1.x <= p2.x ? p1 : p2;
    Point<double>& max = p1.x > p2.x ? p1 : p2;
    double dx = max.x-min.x;
    double dy = max.y-min.y;

    if(dx < 0.01) {
        min = p1.y <= p2.y ? p1 : p2;
        max = p1.y > p2.y ? p1 : p2;
        double x = min.x;
        for(double y = min.y; y < max.y; y +=0.01)
        {
            double cell_x = round(x*100.0);
            double cell_y = round(y*100.0);
            if (isObstacle(cell_x,cell_y)) {
                addPointToList(obstacle_points, x, y);
                if(obstacle_points.size() >= 3)
                    break;
            }
        }
    } else {
        for(double x = min.x; x < max.x; x+=0.01)
        {
            double y = min.y + dy * (x-min.x) / dx;

            double cell_x = round(x*100.0);
            double cell_y = round(y*100.0);
            if (isObstacle(cell_x,cell_y)) {
                addPointToList(obstacle_points, x, y);
                if(obstacle_points.size() >= 3)
                    break;
            }
        }
    }

    response.hit = false;

    if (obstacle_points.size() >= 3) {
        response.hit = true;

        Eigen::Vector2d hit_p(obstacle_points.begin()->x, obstacle_points.begin()->y);

        response.hit_dist = (hit_p - Eigen::Vector2d(p1.x, p1.y)).norm();

        dir.normalize();
        response.hit_x = origin(0) + dir(0)*response.hit_dist;
        response.hit_y = origin(1) + dir(1)*response.hit_dist;
    }

    if (response.hit)
        markers.add_line(p1.x-MAP_X_OFFSET,p1.y-MAP_Y_OFFSET,p2.x-MAP_X_OFFSET,p2.y-MAP_Y_OFFSET,0.1,0.01,255,0,0);
    else
        markers.add_line(p1.x-MAP_X_OFFSET,p1.y-MAP_Y_OFFSET,p2.x-MAP_X_OFFSET,p2.y-MAP_Y_OFFSET,0.1,0.01,0,255,0);

    pub_viz.publish(markers.get());
    markers.clear();

    return true;

}
