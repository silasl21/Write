#include "shaperecognizer.h"
#include <algorithm>
#include <cmath>
#include <numeric>

static double clampUnit(double v)
{
    return std::max(-1.0, std::min(1.0, v));
}

static double pointToSegmentDist(const Point& p, const Point& a, const Point& b)
{
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-12)
        return p.dist(a);
    double t = std::max(0.0, std::min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq));
    Point proj(a.x + t * dx, a.y + t * dy);
    return p.dist(proj);
}

bool ShapeRecognizer::isCircle(const std::vector<Point> &points, Point &centroid, double &avgRadius)
{
    if (points.size() < 10)
        return false;

    double minX = points[0].x, maxX = points[0].x;
    double minY = points[0].y, maxY = points[0].y;
    double pathLen = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        minX = std::min(minX, points[i].x);
        maxX = std::max(maxX, points[i].x);
        minY = std::min(minY, points[i].y);
        maxY = std::max(maxY, points[i].y);
        if (i > 0)
            pathLen += points[i].dist(points[i - 1]);
    }

    double w = maxX - minX;
    double h = maxY - minY;
    if (w < 12.0 || h < 12.0)
        return false;
    if (std::max(w, h) / std::min(w, h) > 1.45)
        return false;

    centroid = { (minX + maxX) * 0.5, (minY + maxY) * 0.5 };

    std::vector<double> distances;
    distances.reserve(points.size());
    for (const auto& point : points) {
        distances.push_back(point.dist(centroid));
    }

    avgRadius = std::accumulate(distances.begin(), distances.end(), 0.0) / distances.size();
    if (avgRadius < 8.0)
        return false;

    if (points.front().dist(points.back()) >= avgRadius * 1.0)
        return false;

    if (pathLen < 2.0 * M_PI * avgRadius * 0.75)
        return false;

    double variance = 0.0;
    for (double d : distances) {
        double diff = d - avgRadius;
        variance += diff * diff;
    }
    variance /= distances.size();

    double stddev = std::sqrt(variance);
    return (stddev / avgRadius) < 0.14;
}

bool ShapeRecognizer::isLine(const std::vector<Point>& points, Point& start, Point& end)
{
    if (points.size() < 4)
        return false;

    start = points.front();
    end = points.back();
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double span = std::hypot(dx, dy);
    if (span < 15.0)
        return false;

    double ux = dx / span;
    double uy = dy / span;
    double sumSq = 0.0;
    double maxPerp = 0.0;

    for (const auto& p : points) {
        double rx = p.x - start.x;
        double ry = p.y - start.y;
        double proj = (rx * ux + ry * uy) / span;
        if (proj < -0.1 || proj > 1.1)
            return false;

        double perp = std::abs(rx * (-uy) + ry * ux);
        sumSq += perp * perp;
        maxPerp = std::max(maxPerp, perp);
    }

    double rmse = std::sqrt(sumSq / points.size());
    return rmse < std::max(2.0, span * 0.05) && maxPerp < std::max(5.0, span * 0.12);
}

bool ShapeRecognizer::isRectangle(const std::vector<Point>& points, std::vector<Point>& outCorners)
{
    if (points.size() < 10)
        return false;

    double minX = points[0].x, maxX = points[0].x;
    double minY = points[0].y, maxY = points[0].y;
    for (const auto& p : points) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }

    double w = maxX - minX;
    double h = maxY - minY;
    if (w < 12.0 || h < 12.0)
        return false;

    Point centroid = { (minX + maxX) * 0.5, (minY + maxY) * 0.5 };

    if (points.front().dist(points.back()) >= std::max(20.0, 0.45 * std::hypot(w, h)))
        return false;

    std::vector<Point> corners(4);
    double maxDists[4] = {0, 0, 0, 0};

    for (const auto& p : points) {
        double dx = p.x - centroid.x;
        double dy = p.y - centroid.y;
        double dist = dx * dx + dy * dy;

        int quadrant = (dy >= 0) * 2 + (dx >= 0);
        if (dist > maxDists[quadrant]) {
            maxDists[quadrant] = dist;
            corners[quadrant] = p;
        }
    }

    for (int q = 0; q < 4; ++q) {
        if (maxDists[q] <= 0.0)
            return false;
    }

    double halfDiag = 0.5 * std::hypot(w, h);
    double avgCornerDist = (corners[0].dist(centroid) + corners[1].dist(centroid) +
                            corners[2].dist(centroid) + corners[3].dist(centroid)) * 0.25;
    if (halfDiag <= 1e-6 || (avgCornerDist / halfDiag) < 0.83)
        return false;

    std::sort(corners.begin(), corners.end(), [&centroid](const Point& a, const Point& b) {
        double angleA = std::atan2(a.y - centroid.y, a.x - centroid.x);
        double angleB = std::atan2(b.y - centroid.y, b.x - centroid.x);
        return angleA < angleB;
    });

    double sides[4];
    double angles[4];
    for (int i = 0; i < 4; ++i) {
        Point a = corners[i];
        Point b = corners[(i + 1) % 4];
        Point c = corners[(i + 2) % 4];

        sides[i] = a.dist(b);
        if (sides[i] < 8.0)
            return false;

        double dx1 = a.x - b.x;
        double dy1 = a.y - b.y;
        double dx2 = c.x - b.x;
        double dy2 = c.y - b.y;

        double mag1 = std::hypot(dx1, dy1);
        double mag2 = std::hypot(dx2, dy2);
        if (mag1 <= 1e-6 || mag2 <= 1e-6)
            return false;

        double dot = dx1 * dx2 + dy1 * dy2;
        angles[i] = std::acos(clampUnit(dot / (mag1 * mag2))) * 180.0 / M_PI;
    }

    bool sidesEqual = std::abs(sides[0] - sides[2]) < 0.30 * ((sides[0] + sides[2]) * 0.5) &&
                      std::abs(sides[1] - sides[3]) < 0.30 * ((sides[1] + sides[3]) * 0.5);
    bool anglesRight = std::all_of(angles, angles + 4, [](double angle) {
        return std::abs(angle - 90.0) < 25.0;
    });
    if (!sidesEqual || !anglesRight)
        return false;

    double edgeDistSum = 0.0;
    for (const auto& p : points) {
        double minEdgeDist = pointToSegmentDist(p, corners[0], corners[1]);
        for (int i = 1; i < 4; ++i)
            minEdgeDist = std::min(minEdgeDist, pointToSegmentDist(p, corners[i], corners[(i + 1) % 4]));
        edgeDistSum += minEdgeDist;
    }
    double avgEdgeDist = edgeDistSum / points.size();
    if (avgEdgeDist >= std::max(4.0, 0.14 * std::min(w, h)))
        return false;

    outCorners = corners;
    return true;
}

bool ShapeRecognizer::isArrow(const std::vector<Point>& points, Point& start, Point& end)
{
    if (points.size() < 8)
        return false;

    size_t tipIdx = points.size() / 2;
    double maxDist = -1.0;
    for (size_t i = points.size() / 2; i + 1 < points.size(); ++i) {
        double d = points[i].dist(points.front());
        if (d > maxDist) {
            maxDist = d;
            tipIdx = i;
        }
    }

    std::vector<Point> shaft(points.begin(), points.begin() + tipIdx + 1);
    std::vector<Point> head(points.begin() + tipIdx, points.end());
    if (shaft.size() < 4 || head.size() < 3)
        return false;

    if (!isLine(shaft, start, end))
        return false;

    double shaftLen = start.dist(end);
    if (shaftLen < 20.0)
        return false;

    double headDx = points.back().x - end.x;
    double headDy = points.back().y - end.y;
    double headLen = std::hypot(headDx, headDy);
    if (headLen < std::max(8.0, 0.08 * shaftLen) || headLen > 0.65 * shaftLen)
        return false;

    double ux = (end.x - start.x) / shaftLen;
    double uy = (end.y - start.y) / shaftLen;
    double vx = headDx / headLen;
    double vy = headDy / headLen;

    double angleDeg = std::acos(clampUnit(ux * vx + uy * vy)) * 180.0 / M_PI;
    return angleDeg >= 115.0 && angleDeg <= 170.0;
}
