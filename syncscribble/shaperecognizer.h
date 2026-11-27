#ifndef SHAPERECOGNIZER_H
#define SHAPERECOGNIZER_H

#include <vector>
#include "strokebuilder.h"

class ShapeRecognizer {
public:
    enum Shape {
      None,
      Circle,
      Ellipse,
      Rectangle,
      Triangle,
      Arrow,
      CurvedArrow,
      Arc,
      Line
    };

    static bool isCircle(const std::vector<Point>& points, Point& centroid, double& avgRadius);
    static bool isEllipse(const std::vector<Point>& points, Point& center, double& rx, double& ry, double& angle);
    static bool isLine(const std::vector<Point>& points, Point& start, Point& end);
    static bool isRectangle(const std::vector<Point>& points, std::vector<Point>& outCorners);
    static bool isTriangle(const std::vector<Point>& points, Point outVerts[3]);
    static bool isArrow(const std::vector<Point>& points, Point& start, Point& end);
    static bool isArc(const std::vector<Point>& points, Point& start, Point& end, double& bulge);
    static bool isCurvedArrow(const std::vector<Point>& points, Point& start, Point& end, double& bulge);
    static Point snapLineAngle(Point start, Point end, double thresholdDeg = 6.0);
    static bool calcArcGeometry(Point start, Point end, double bulge,
        Point& center, double& radius, double& startAngle, double& sweepAngle);
};

#endif
