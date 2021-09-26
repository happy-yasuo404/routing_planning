/**
 * @file frenet_optimal_trajectory.cpp
 * @author your name (you@domain.com)
 * @brief 
 * @version 0.1
 * @date 2021-09-20
 * 
 * @copyright Copyright (c) 2021
 * 
 */
#include <iostream>
#include <limits>
#include <vector>
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <sys/time.h>
#include "cubic_spline.h"
#include "frenet_path.h"
#include "quintic_polynomial.h"
#include "quartic_polynomial.h"

// 速度参数
#define SIM_LOOP 500
#define MAX_SPEED 50.0 / 3.6 // 最大速度 [m/s]
#define MAX_ACCEL 2.0        // 最大加速度 [m/(s*s)]

// 1/R R越小 该值越大 最小转向旋转半径
#define MAX_CURVATURE 1.0  // 最大曲率 [1/m]
#define MAX_ROAD_WIDTH 7.0 // 最大道路长度 [m]
#define D_ROAD_W 1.0       // 道路横向采样长度 [m]
// 预测时间
#define DT 0.2 // 采样间隔 [s]
// 预测的最长时间
#define MAXT 5.0 // max prediction time[m]
// 预测的最短时间
#define MINT 4.0 // min prediction time[m]

// 横向速度预期
#define TARGET_SPEED 30.0 / 3.6 // 目标速度 [m/s]
#define D_T_S 5.0 / 3.6         // 目标速度采样长度[m/s]
#define N_S_SAMPLE 1            // 目标速度采样数

#define ROBOT_RADIUS 1.5 // robot radius [m]

// #惩罚jerk
#define KJ 0.1
// #制动时间
#define KT 0.1
// #偏离中心线
#define KD 1.0
#define KLAT 1.0
#define KLON 1.0

using namespace cpprobotics;

// 求各种代价的总和
float sum_of_power(std::vector<float> value_list)
{
  float sum = 0;
  for (float item : value_list)
  {
    sum += item * item;
  }
  return sum;
};

/**
 * @brief 返回筛选出路径的代价，统一存放
 * 
 * @param c_speed 
 * @param c_d 
 * @param c_d_d 
 * @param c_d_dd 
 * @param s0 
 * @return Vec_Path 
 */
Vec_Path calc_frenet_paths(float c_speed, float c_d, float c_d_d, float c_d_dd, float s0)
{
  std::vector<FrenetPath> fp_list;
  for (float di = -1 * MAX_ROAD_WIDTH; di < MAX_ROAD_WIDTH; di += D_ROAD_W)
  {
    for (float Ti = MINT; Ti < MAXT; Ti += DT)
    {
      FrenetPath fp;
      // QuinticPolynomial(float xs_, float vxs_, float axs_, float xe_, float vxe_, float axe_, float T) : xs(xs_), vxs(vxs_), axs(axs_), xe(xe_), vxe(vxe_), axe(axe_), a0(xs_), a1(vxs_), a2(axs_ / 2.0)
      QuinticPolynomial lat_qp(c_d, c_d_d, c_d_dd, di, 0.0, 0.0, Ti);
      for (float t = 0; t < Ti; t += DT)
      {
        // 压入时间
        fp.t.push_back(t);
        // 压入x(t)
        fp.d.push_back(lat_qp.calc_point(t));
        // 压入v(t)
        fp.d_d.push_back(lat_qp.calc_first_derivative(t));
        // 压入a(t)
        fp.d_dd.push_back(lat_qp.calc_second_derivative(t));
        // 压入da(t)
        fp.d_ddd.push_back(lat_qp.calc_third_derivative(t));
      }

      for (float tv = TARGET_SPEED - D_T_S * N_S_SAMPLE;
           tv < TARGET_SPEED + D_T_S * N_S_SAMPLE;
           tv += D_T_S)
      {
        // 依据纵向距离来进行横向采样
        FrenetPath fp_bot = fp;
        QuarticPolynomial lon_qp(s0, c_speed, 0.0, tv, 0.0, Ti);
        // std::numeric_limits<float>::min()计算浮点的最小值
        fp_bot.max_speed = std::numeric_limits<float>::min();
        fp_bot.max_accel = std::numeric_limits<float>::min();

        // 以纵坐标来计算横向
        for (float t_ : fp.t)
        {
          // 压入横向多项式
          fp_bot.s.push_back(lon_qp.calc_point(t_));
          fp_bot.s_d.push_back(lon_qp.calc_first_derivative(t_));
          fp_bot.s_dd.push_back(lon_qp.calc_second_derivative(t_));
          fp_bot.s_ddd.push_back(lon_qp.calc_third_derivative(t_));
          // 更新最大横向速度
          if (fp_bot.s_d.back() > fp_bot.max_speed)
          {
            fp_bot.max_speed = fp_bot.s_d.back();
          }
          // 更新最大横向加速度
          if (fp_bot.s_dd.back() > fp_bot.max_accel)
          {
            fp_bot.max_accel = fp_bot.s_dd.back();
          }
        }

        // 计算jeck
        float Jp = sum_of_power(fp.d_ddd);
        float Js = sum_of_power(fp_bot.s_ddd);
        float ds = (TARGET_SPEED - fp_bot.s_d.back());

        fp_bot.cd = KJ * Jp + KT * Ti + KD * std::pow(fp_bot.d.back(), 2);
        fp_bot.cv = KJ * Js + KT * Ti + KD * ds;
        fp_bot.cf = KLAT * fp_bot.cd + KLON * fp_bot.cv;

        fp_list.push_back(fp_bot);
        fp_list.push_back(fp_bot);
      }
    }
  }
  return fp_list;
};

/**
 * @brief 转换成全局路径
 * 
 * @param path_list 产选出的路径
 * @param csp 插值离散路径点
 */
void calc_global_paths(Vec_Path &path_list, Spline2D csp)
{
  for (Vec_Path::iterator path_p = path_list.begin(); path_p != path_list.end(); path_p++)
  {
    for (unsigned int i = 0; i < path_p->s.size(); i++)
    {
      // 如果弧长比最大弧长还大，那说明已经离开轨迹，直接终止
      if (path_p->s[i] >= csp.s.back())
      {
        break;
      }
      // 记录实际路径s对应路径离散点的x，y，yaw,
      std::array<float, 2> poi = csp.calc_postion(path_p->s[i]);
      float iyaw = csp.calc_yaw(path_p->s[i]);

      // 转换到笛卡尔坐标系
      float di = path_p->d[i];
      float x = poi[0] + di * std::cos(iyaw + M_PI / 2.0);
      float y = poi[1] + di * std::sin(iyaw + M_PI / 2.0);
      path_p->x.push_back(x);
      path_p->y.push_back(y);
    }

    for (int i = 0; i < path_p->x.size() - 1; i++)
    {
      float dx = path_p->x[i + 1] - path_p->x[i];
      float dy = path_p->y[i + 1] - path_p->y[i];
      path_p->yaw.push_back(std::atan2(dy, dx));
      path_p->ds.push_back(std::sqrt(dx * dx + dy * dy));
    }

    path_p->yaw.push_back(path_p->yaw.back());
    path_p->ds.push_back(path_p->ds.back());

    path_p->max_curvature = std::numeric_limits<float>::min();
    for (int i = 0; i < path_p->x.size() - 1; i++)
    {
      path_p->c.push_back((path_p->yaw[i + 1] - path_p->yaw[i]) / path_p->ds[i]);
      if (path_p->c.back() > path_p->max_curvature)
      {
        path_p->max_curvature = path_p->c.back();
      }
    }
  }
};

// 检查碰撞
bool check_collision(FrenetPath path, const Vec_Poi ob)
{
  for (auto point : ob)
  {
    for (unsigned int i = 0; i < path.x.size(); i++)
    {
      float dist = std::pow((path.x[i] - point[0]), 2) + std::pow((path.y[i] - point[1]), 2);
      if (dist <= ROBOT_RADIUS * ROBOT_RADIUS)
      {
        return false;
      }
    }
  }
  return true;
};

// 筛选路径
Vec_Path check_paths(Vec_Path path_list, const Vec_Poi ob)
{
  Vec_Path output_fp_list;
  for (FrenetPath path : path_list)
  {
    // 筛选速度、加速度、曲率和碰撞
    if (path.max_speed < MAX_SPEED && path.max_accel < MAX_ACCEL && path.max_curvature < MAX_CURVATURE && check_collision(path, ob))
    {
      output_fp_list.push_back(path);
    }
  }
  return output_fp_list;
};

// frenet 最优规划
FrenetPath frenet_optimal_planning(
    Spline2D csp, float s0, float c_speed,
    float c_d, float c_d_d, float c_d_dd, Vec_Poi ob)
{
  Vec_Path fp_list = calc_frenet_paths(c_speed, c_d, c_d_d, c_d_dd, s0);
  calc_global_paths(fp_list, csp);
  Vec_Path save_paths = check_paths(fp_list, ob);

  float min_cost = std::numeric_limits<float>::max();
  FrenetPath final_path;
  for (auto path : save_paths)
  {
    if (min_cost >= path.cf)
    {
      min_cost = path.cf;
      final_path = path;
    }
  }
  return final_path;
};

// 刷新动画
cv::Point2i cv_offset(
    float x, float y, int image_width = 2000, int image_height = 2000)
{
  cv::Point2i output;
  output.x = int(x * 100) + 300;
  output.y = image_height - int(y * 100) - image_height / 3;
  return output;
};

int main()
{
  Vec_f wx({0.0, 10.0, 20.5, 35.0, 70.5});
  Vec_f wy({0.0, -6.0, 5.0, 6.5, 0.0});
  std::vector<Poi_f> obstcles{
      {{20.0, 10.0}},
      {{30.0, 6.0}},
      {{30.0, 8.0}},
      {{35.0, 8.0}},
      {{50.0, 3.0}}};

  Spline2D csp_obj(wx, wy);
  Vec_f r_x;
  Vec_f r_y;
  Vec_f ryaw;
  Vec_f rcurvature;
  Vec_f rs;

  for (float i = 0; i < csp_obj.s.back(); i += 0.1)
  {
    std::array<float, 2> point_ = csp_obj.calc_postion(i);
    r_x.push_back(point_[0]);
    r_y.push_back(point_[1]);
    ryaw.push_back(csp_obj.calc_yaw(i));
    rcurvature.push_back(csp_obj.calc_curvature(i));
    rs.push_back(i);
  }

  float c_speed = 10.0 / 3.6;
  float c_d = 2.0;
  float c_d_d = 0.0;
  float c_d_dd = 0.0;
  float s0 = 0.0;

  float area = 20.0;

  cv::namedWindow("frenet", cv::WINDOW_NORMAL);
  int count = 0;

  for (int i = 0; i < SIM_LOOP; i++)
  {
    FrenetPath final_path = frenet_optimal_planning(csp_obj, s0, c_speed, c_d, c_d_d, c_d_dd, obstcles);

    s0 = final_path.s[1];
    c_d = final_path.d[1];
    c_d_d = final_path.d_d[1];
    c_d_dd = final_path.d_dd[1];
    c_speed = final_path.s_d[1];

    if (std::pow((final_path.x[1] - r_x.back()), 2) + std::pow((final_path.y[1] - r_y.back()), 2) <= 1.0)
    {
      break;
    }

    // visualization`
    cv::Mat bg(2000, 8000, CV_8UC3, cv::Scalar(255, 255, 255));
    for (unsigned int i = 1; i < r_x.size(); i++)
    {
      cv::line(
          bg,
          cv_offset(r_x[i - 1], r_y[i - 1], bg.cols, bg.rows),
          cv_offset(r_x[i], r_y[i], bg.cols, bg.rows),
          cv::Scalar(0, 0, 0),
          10);
    }
    for (unsigned int i = 0; i < final_path.x.size(); i++)
    {
      cv::circle(
          bg,
          cv_offset(final_path.x[i], final_path.y[i], bg.cols, bg.rows),
          40, cv::Scalar(255, 0, 0), -1);
    }

    cv::circle(
        bg,
        cv_offset(final_path.x.front(), final_path.y.front(), bg.cols, bg.rows),
        50, cv::Scalar(0, 255, 0), -1);

    for (unsigned int i = 0; i < obstcles.size(); i++)
    {
      cv::circle(
          bg,
          cv_offset(obstcles[i][0], obstcles[i][1], bg.cols, bg.rows),
          40, cv::Scalar(0, 0, 255), 5);
    }

    cv::putText(
        bg,
        "Speed: " + std::to_string(c_speed * 3.6).substr(0, 4) + "km/h",
        cv::Point2i((int)bg.cols * 0.5, (int)bg.rows * 0.1),
        cv::FONT_HERSHEY_SIMPLEX,
        5,
        cv::Scalar(0, 0, 0),
        10);

    cv::imshow("frenet", bg);
    cv::waitKey(5);

    // save image in build/bin/pngs
    // struct timeval tp;
    // gettimeofday(&tp, NULL);
    // long int ms = tp.tv_sec * 1000 + tp.tv_usec / 1000;
    // std::string int_count = std::to_string(ms);
    // cv::imwrite("./pngs/"+int_count+".png", bg);
  }
  return 0;
};
