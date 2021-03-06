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
    srv_fit = handle.advertiseService("/mapping/fitblob", &Mapping::serviceFitRequest, this);

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

    //updateWalls();
}

//void Mapping::markPointsBetween(Point<double> p1, Point<double> p2, double val)
//{
//    Point<double> min = p1.x <= p2.x ? p1 : p2;
//    Point<double> max = p1.x > p2.x ? p1 : p2;
//    double dx = max.x-min.x;
//    double dy = max.y-min.y;

//    if(std::abs(dx) < 0.01) {
//        min = p1.y <= p2.y ? p1 : p2;
//        max = p1.y > p2.y ? p1 : p2;
//        double x = min.x;
//        for(double y = min.y; y < max.y; y +=0.01)
//        {
//            markPoint(Point<double>(x,y),val);
//        }
//    } else {
//        for(double x = min.x; x < max.x; x += 0.001)
//        {
//            double y = min.y + dy * (x-min.x) / dx;
//            markPoint(Point<double>(x,y),val);
//        }
//    }
//}


/**
  * Adapted from
  * https://github.com/clearpathrobotics/occupancy_grid_utils/blob/hydro-devel/include/occupancy_grid_utils/impl/ray_trace_iterator.h
  */
void Mapping::markPointsBetween(Point<int> p0, Point<int> p1, double val)
{
    const int dx = p1.x-p0.x;
    const int dy = p1.y-p0.y;
    const int abs_dx = abs(dx);
    const int abs_dy = abs(dy);
    const int8_t offset_dx = (dx>0) ? 1 : -1;;
    const int8_t offset_dy = (dy>0) ? 1 : -1;;

    int x_inc, y_inc;
    int x_correction, y_correction;
    int error, error_inc, error_threshold;

    if (abs_dx > abs_dy) {
        x_inc = offset_dx;
        y_inc = 0;
        x_correction = 0;
        y_correction = offset_dy;
        error = abs_dx/2;
        error_inc = abs_dy;
        error_threshold = abs_dx;
    }
    else {
        x_inc = 0;
        y_inc = offset_dy;
        x_correction = offset_dx;
        y_correction = 0;
        error = abs_dy/2;
        error_inc = abs_dx;
        error_threshold = abs_dy;
    }

    Point<int> cell(p0.x,p0.y);

    markProbabilityGrid(cell, val);
    updateOccupancyGrid(cell);

    while (cell.x != p1.x || cell.y != p1.y)
    {
        cell.x += x_inc;
        cell.y += y_inc;
        error += error_inc;
        if (error >= error_threshold) {
            cell.x += x_correction;
            cell.y += y_correction;
            error -= error_threshold;
        }

        markProbabilityGrid(cell, val);
        updateOccupancyGrid(cell);
    }
}


void Mapping::updateIR(double ir_reading, double ir_x_offset)
{
    Point<double> ir_pos = Point<double>(ir_x_offset, 0);
    Point<int> p0 = robotPointToCell(ir_pos);

    if(isIRValid(ir_reading))
    {
        Point<double> obstacle(ir_x_offset, ir_reading);
        Point<int> p1 = robotPointToCell(obstacle);

        markCellOccupied(p1);
        markPointsBetween(p0, p1, P_FREE);
    } else {
        double mult = ir_reading > 0 ? 1.0 : -1.0;
        Point<double> max_point = Point<double>(ir_pos.x, MAX_IR_DIST*0.75*mult);
        Point<int> p1 = robotPointToCell(max_point);
        markPointsBetween(p0, p1, P_FREE);
    }
}

void Mapping::updateWalls()
{
    for(int i = 0; i < wall_planes->size(); ++i)
    {
        if(wall_planes->at(i).is_ground_plane())
            continue;

        common::vision::SegmentedPlane& plane = wall_planes->at(i);

        double width = std::max(plane.get_obb().get_width(), plane.get_obb().get_depth());
        //ROS_ERROR("Wall w=%.3lf, h=%.3lf, d=%.3lf",plane.get_obb().get_width(), plane.get_obb().get_height(), plane.get_obb().get_depth());

        //only consider walls that have a minimum width
        if(width < 0.2)
            continue;

        //extract start and end position
        Eigen::Vector2f center = plane.get_obb().get_translation().head<2>();

        const pcl::ModelCoefficients::ConstPtr& coeff = plane.get_coefficients();
        Eigen::Vector2f normal(coeff->values[0],coeff->values[1]);
        normal.normalize();

        Eigen::Vector2f ortho(-normal(1),normal(0));

        Eigen::Vector2f p0 = center + ortho*width;
        Eigen::Vector2f p1 = center - ortho*width;

        Point<int> cell_p0 = robotPointToCell(Point<double>(p0(0),p0(1)));
        Point<int> cell_p1 = robotPointToCell(Point<double>(p1(0),p1(1)));
        markPointsBetween(cell_p0, cell_p1, P_OCC);
    }

}

/*void Mapping::markPointOccupied(Point<double> point)
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
}*/

void Mapping::markCellOccupied(Point<int> cell, int neighborhood)
{
    int x = cell.x;
    int y = cell.y;
    Point<int> c;
    for(int i = x-neighborhood; i < x+neighborhood; ++i)
    {
        for(int j = y-neighborhood; j < y+neighborhood; ++j)
        {
            c.x = i;
            c.y = j;
            markProbabilityGrid(c, P_OCC);
            updateOccupancyGrid(c);
        }
    }
}

void Mapping::markPoint(Point<double> point, double val)
{
    Point<int> cell = robotPointToCell(point);
    markProbabilityGrid(cell, val);
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
    if (cell.y < 0 || cell.y >= prob_grid.size() ||
        cell.x < 0 || cell.x >= prob_grid[cell.y].size())
    {
        ROS_ERROR("[Mapping::markProbabilityGrid] Array out of bounds");
        return;
    }

    prob_grid[cell.y][cell.x] += log_prob - P_PRIOR;
}

void Mapping::updateOccupancyGrid(Point<int> cell)
{
    if (cell.y < 0 || cell.y >= prob_grid.size() ||
        cell.x < 0 || cell.x >= prob_grid[cell.y].size())
    {
        ROS_ERROR("[Mapping::markProbabilityGrid] Array out of bounds");
        return;
    }

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

    markers.add(msg);
    pub_viz.publish(markers.get());
    markers.clear();

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

Point<double> Mapping::transformPointToRobotSystem(std::string& frame_id, double x, double y)
{
    if (frame_id.compare("robot")==0)
        return Point<double>(x,y);

    geometry_msgs::PointStamped stamped_in;
    stamped_in.header.frame_id = frame_id;
    stamped_in.point.x = x;
    stamped_in.point.y = y;
    stamped_in.point.z = 0;

    geometry_msgs::PointStamped stamped_out;
    tf_listener.transformPoint("robot",stamped_in,stamped_out);

    return Point<double>(stamped_out.point.x, stamped_out.point.y);
}

Point<int> Mapping::transformPointToGridSystem(std::string& frame_id, double x, double y)
{
    geometry_msgs::PointStamped stamped_in;
    stamped_in.header.frame_id = frame_id;
    //stamped_in.header.stamp = ros::Time::now();
    stamped_in.header.stamp = transform.stamp_;
    stamped_in.point.x = x;
    stamped_in.point.y = y;
    stamped_in.point.z = 0;

    geometry_msgs::PointStamped stamped_out;
    tf_listener.transformPoint("map",stamped_in,stamped_out);

    return mapPointToCell(Point<double>(stamped_out.point.x + MAP_X_OFFSET, stamped_out.point.y + MAP_Y_OFFSET));
}

Point<double> Mapping::transformCellToMap(Point<int>& cell)
{
    double x = cell.x;
    double y = cell.y;
    return Point<double>(x/100.0 - MAP_X_OFFSET, y/100.0 - MAP_Y_OFFSET);
}

bool Mapping::performRaycast(navigation_msgs::RaycastRequest &request, navigation_msgs::RaycastResponse &response)
{
    Eigen::Vector2d dir(request.dir_x, request.dir_y);
    dir.normalize();
    dir *= request.max_length;

    //transform points to robot coordinate system (better would be map system though.)
    Point<int> p0 = transformPointToGridSystem(request.frame_id, request.origin_x, request.origin_y);
    Point<int> p1 = transformPointToGridSystem(request.frame_id, request.origin_x + dir(0), request.origin_y + dir(1));

    std::vector<Point<double> > obstacle_points;

    //raycast algorithm, adapted from markPointsBetween
    const int dx = p1.x-p0.x;
    const int dy = p1.y-p0.y;
    const int abs_dx = abs(dx);
    const int abs_dy = abs(dy);
    const int8_t offset_dx = (dx>0) ? 1 : -1;;
    const int8_t offset_dy = (dy>0) ? 1 : -1;;

    int x_inc, y_inc;
    int x_correction, y_correction;
    int error, error_inc, error_threshold;

    if (abs_dx > abs_dy) {
        x_inc = offset_dx;
        y_inc = 0;
        x_correction = 0;
        y_correction = offset_dy;
        error = abs_dx/2;
        error_inc = abs_dy;
        error_threshold = abs_dx;
    }
    else {
        x_inc = 0;
        y_inc = offset_dy;
        x_correction = offset_dx;
        y_correction = 0;
        error = abs_dy/2;
        error_inc = abs_dx;
        error_threshold = abs_dy;
    }

    Point<int> cell(p0.x,p0.y);

    if (isObstacle(cell.x,cell.y)) {
        obstacle_points.push_back(transformCellToMap(cell));
    }

    while (cell.x != p1.x || cell.y != p1.y)
    {
        cell.x += x_inc;
        cell.y += y_inc;
        error += error_inc;
        if (error >= error_threshold) {
            cell.x += x_correction;
            cell.y += y_correction;
            error -= error_threshold;
        }

//        markProbabilityGrid(cell,P_OCC);
//        updateOccupancyGrid(cell);

        if (isObstacle(cell.x,cell.y)) {
            obstacle_points.push_back(transformCellToMap(cell));

            if(obstacle_points.size() >= 2)
                break;
        }
    }

    //TODO: distance between 3 hits cannot be greater than x

    response.hit = false;

    Point<double> p0_map = transformCellToMap(p0);

    if (obstacle_points.size() >= 2) {
        response.hit = true;

        double x_avg = 0;
        double y_avg = 0;
        for(int i = 0; i < obstacle_points.size(); ++i)
        {
            x_avg += obstacle_points[i].x;
            y_avg += obstacle_points[i].y;
        }
        x_avg /= (double)obstacle_points.size();
        y_avg /= (double)obstacle_points.size();

        Eigen::Vector2d hit_p(x_avg, y_avg);
        Eigen::Vector2d origin(p0_map.x, p0_map.y);

        response.hit_dist = (hit_p - origin).norm();

        dir.normalize();
        response.hit_x = hit_p(0);
        response.hit_y = hit_p(1);
    }

    if (response.hit) {
        Point<double> map_p1(response.hit_x, response.hit_y);
        markers.add_line(p0_map.x,p0_map.y, map_p1.x,map_p1.y,0.1,0.01,255,0,0);
    }
    else {
        Point<double> p1_map = transformCellToMap(p1);
        markers.add_line(p0_map.x,p0_map.y, p1_map.x,p1_map.y,0.1,0.01,0,255,0);
    }

    pub_viz.publish(markers.get());
    markers.clear();

    return true;

}

bool Mapping::serviceFitRequest(navigation_msgs::FitBlobRequest &request, navigation_msgs::FitBlobResponse &response)
{
    double radius_map = request.radius;
    int radius = round(radius_map*100.0);
    Point<int> center = transformPointToGridSystem(request.frame_id, request.x, request.y);
    Point<int> top_left(center.x - radius, center.y - radius);
    int width = radius*2;

    int num_cells = 0;
    int num_occluded = 0;

    //count occluded cells
    for(int y = top_left.y; y < top_left.y+width; ++y) {
        for(int x = top_left.x; x < top_left.x+width; ++x) {

            int dx = center.x - x; // horizontal offset
            int dy = center.y - y; // vertical offset
            if ( (dx*dx + dy*dy) <= (radius*radius) )
            {
                if (isObstacle(x,y))
                    num_occluded++;

                num_cells++;

//                Point<int> cell(x,y);
//                markProbabilityGrid(cell,P_OCC);
//                updateOccupancyGrid(cell);
            }

        }
    }

    response.fits = ((double)num_occluded/(double)num_cells) < request.max_occlusion_ratio;

    return true;

}
