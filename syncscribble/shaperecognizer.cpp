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
    if (std::max(w, h) / std::min(w, h) > 1.18)
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

static double triangleArea2(const Point& a, const Point& b, const Point& c)
{
    return std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

Point ShapeRecognizer::snapLineAngle(Point start, Point end, double thresholdDeg)
{
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double len = std::hypot(dx, dy);
    if (len <= 1e-6)
        return end;
    double angle = std::atan2(dy, dx);
    double step = M_PI / 4.0;
    double nearest = std::round(angle / step) * step;
    double diffDeg = std::abs(angle - nearest) * 180.0 / M_PI;
    if (diffDeg <= thresholdDeg)
        return Point(start.x + len * std::cos(nearest), start.y + len * std::sin(nearest));
    return end;
}

bool ShapeRecognizer::isTriangle(const std::vector<Point>& points, Point outVerts[3])
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
    double diag = std::hypot(w, h);
    if (w < 12.0 || h < 12.0 || diag < 18.0)
        return false;

    if (points.front().dist(points.back()) >= std::max(22.0, 0.45 * diag))
        return false;

    Point center((minX + maxX) * 0.5, (minY + maxY) * 0.5);
    Point v0 = points[0];
    double maxD = -1.0;
    for (const auto& p : points) {
        double d = p.dist(center);
        if (d > maxD) {
            maxD = d;
            v0 = p;
        }
    }

    Point v1 = v0;
    maxD = -1.0;
    for (const auto& p : points) {
        double d = p.dist(v0);
        if (d > maxD) {
            maxD = d;
            v1 = p;
        }
    }

    Point v2 = v0;
    double maxArea = -1.0;
    for (const auto& p : points) {
        double a = triangleArea2(v0, v1, p);
        if (a > maxArea) {
            maxArea = a;
            v2 = p;
        }
    }

    Point verts[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; ++k) {
        const Point& a = verts[(k + 1) % 3];
        const Point& b = verts[(k + 2) % 3];
        double best = -1.0;
        for (const auto& p : points) {
            double area = triangleArea2(a, b, p);
            if (area > best) {
                best = area;
                verts[k] = p;
            }
        }
    }

    double perim = 0.0;
    for (int i = 0; i < 3; ++i) {
        Point a = verts[i];
        Point b = verts[(i + 1) % 3];
        Point c = verts[(i + 2) % 3];
        double side = a.dist(b);
        if (side < 12.0)
            return false;
        perim += side;

        double dx1 = a.x - b.x, dy1 = a.y - b.y;
        double dx2 = c.x - b.x, dy2 = c.y - b.y;
        double m1 = std::hypot(dx1, dy1), m2 = std::hypot(dx2, dy2);
        if (m1 <= 1e-6 || m2 <= 1e-6)
            return false;
        double ang = std::acos(clampUnit((dx1 * dx2 + dy1 * dy2) / (m1 * m2))) * 180.0 / M_PI;
        if (ang < 20.0 || ang > 135.0)
            return false;

        // Require actual stroke coverage along the middle third of every side (rejects open Vs)
        Point mStart(a.x + (b.x - a.x) / 3.0, a.y + (b.y - a.y) / 3.0);
        Point mEnd(a.x + 2.0 * (b.x - a.x) / 3.0, a.y + 2.0 * (b.y - a.y) / 3.0);
        double minMidDist = 1e9;
        for (const auto& p : points)
            minMidDist = std::min(minMidDist, pointToSegmentDist(p, mStart, mEnd));
        if (minMidDist > std::max(8.0, 0.18 * side))
            return false;
    }

    if (pathLen < 0.78 * perim)
        return false;

    double distSum = 0.0;
    double maxEdgeDist = 0.0;
    for (const auto& p : points) {
        double d = pointToSegmentDist(p, verts[0], verts[1]);
        d = std::min(d, pointToSegmentDist(p, verts[1], verts[2]));
        d = std::min(d, pointToSegmentDist(p, verts[2], verts[0]));
        distSum += d;
        maxEdgeDist = std::max(maxEdgeDist, d);
    }

    double avgEdgeDist = distSum / points.size();
    if (avgEdgeDist >= std::max(4.0, 0.11 * std::min(w, h)))
        return false;
    if (maxEdgeDist >= std::max(9.0, 0.18 * diag))
        return false;

    for (int i = 0; i < 3; ++i)
        outVerts[i] = verts[i];
    return true;
}

bool ShapeRecognizer::isEllipse(const std::vector<Point>& points, Point& center, double& rx, double& ry, double& angle)
{
    if (points.size() < 12)
        return false;

    double minX = points[0].x, maxX = points[0].x;
    double minY = points[0].y, maxY = points[0].y;
    std::vector<double> cumLen(points.size(), 0.0);
    for (size_t i = 0; i < points.size(); ++i) {
        minX = std::min(minX, points[i].x);
        maxX = std::max(maxX, points[i].x);
        minY = std::min(minY, points[i].y);
        maxY = std::max(maxY, points[i].y);
        if (i > 0)
            cumLen[i] = cumLen[i - 1] + points[i].dist(points[i - 1]);
    }

    double pathLen = cumLen.back();
    double w = maxX - minX, h = maxY - minY;
    double diag = std::hypot(w, h);
    if (w < 12.0 || h < 12.0 || diag < 20.0 || pathLen < 24.0)
        return false;
    if (points.front().dist(points.back()) >= std::max(24.0, 0.48 * diag))
        return false;

    // Resample uniformly along arc length so slow pen sections don't skew PCA
    const int N = 64;
    std::vector<Point> samples;
    samples.reserve(N);
    size_t seg = 0;
    for (int k = 0; k < N; ++k) {
        double target = (pathLen * k) / (N - 1);
        while (seg + 1 < points.size() && cumLen[seg + 1] < target)
            ++seg;
        if (seg + 1 >= points.size()) {
            samples.push_back(points.back());
        } else {
            double segLen = cumLen[seg + 1] - cumLen[seg];
            double t = segLen > 1e-9 ? (target - cumLen[seg]) / segLen : 0.0;
            samples.push_back(Point(points[seg].x + t * (points[seg + 1].x - points[seg].x),
                                    points[seg].y + t * (points[seg + 1].y - points[seg].y)));
        }
    }

    Point pcaCenter(0, 0);
    for (const auto& p : samples) {
        pcaCenter.x += p.x;
        pcaCenter.y += p.y;
    }
    pcaCenter.x /= N;
    pcaCenter.y /= N;

    double cxx = 0.0, cyy = 0.0, cxy = 0.0;
    for (const auto& p : samples) {
        double dx = p.x - pcaCenter.x;
        double dy = p.y - pcaCenter.y;
        cxx += dx * dx;
        cyy += dy * dy;
        cxy += dx * dy;
    }

    angle = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
    double step90 = M_PI * 0.5;
    double nearest90 = std::round(angle / step90) * step90;
    if (std::abs(angle - nearest90) * 180.0 / M_PI <= 10.0)
        angle = nearest90;

    double ca = std::cos(angle);
    double sa = std::sin(angle);
    double minLx = 1e9, maxLx = -1e9, minLy = 1e9, maxLy = -1e9;
    for (const auto& p : points) {
        double dx = p.x - pcaCenter.x;
        double dy = p.y - pcaCenter.y;
        double lx = dx * ca + dy * sa;
        double ly = -dx * sa + dy * ca;
        minLx = std::min(minLx, lx);
        maxLx = std::max(maxLx, lx);
        minLy = std::min(minLy, ly);
        maxLy = std::max(maxLy, ly);
    }

    double midLx = 0.5 * (minLx + maxLx);
    double midLy = 0.5 * (minLy + maxLy);
    center = Point(pcaCenter.x + midLx * ca - midLy * sa,
                   pcaCenter.y + midLx * sa + midLy * ca);

    rx = 0.5 * (maxLx - minLx);
    ry = 0.5 * (maxLy - minLy);
    if (rx < 8.0 || ry < 8.0)
        return false;
    if (std::max(rx, ry) / std::min(rx, ry) > 4.0)
        return false;

    double ramanujanP = M_PI * (3.0 * (rx + ry) - std::sqrt((3.0 * rx + ry) * (rx + 3.0 * ry)));
    if (pathLen < 0.75 * ramanujanP)
        return false;

    double sumR = 0.0, sumErrSq = 0.0;
    for (const auto& p : points) {
        double dx = p.x - center.x;
        double dy = p.y - center.y;
        double lx = dx * ca + dy * sa;
        double ly = -dx * sa + dy * ca;
        double rn = std::hypot(lx / rx, ly / ry);
        sumR += rn;
        double err = rn - 1.0;
        sumErrSq += err * err;
    }
    double meanR = sumR / points.size();
    double rmsErr = std::sqrt(sumErrSq / points.size());
    return meanR >= 0.88 && meanR <= 1.12 && rmsErr < 0.14;
}

bool ShapeRecognizer::calcArcGeometry(Point start, Point end, double bulge,
    Point& center, double& radius, double& startAngle, double& sweepAngle)
{
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double L = std::hypot(dx, dy);
    if (L <= 1e-6 || std::abs(bulge) <= 1e-4)
        return false;

    double h = L * 0.5;
    double s = bulge * L;
    double nx = -dy / L;
    double ny = dx / L;
    Point mid((start.x + end.x) * 0.5, (start.y + end.y) * 0.5);
    Point apex(mid.x + nx * s, mid.y + ny * s);

    radius = (h * h + s * s) / (2.0 * std::abs(s));
    double dCenter = (s * s - h * h) / (2.0 * s);
    center = Point(mid.x + nx * dCenter, mid.y + ny * dCenter);
    startAngle = std::atan2(start.y - center.y, start.x - center.x);

    double magSweep = 4.0 * std::atan(2.0 * std::abs(bulge));
    Point midPos(center.x + radius * std::cos(startAngle + magSweep * 0.5),
                 center.y + radius * std::sin(startAngle + magSweep * 0.5));
    Point midNeg(center.x + radius * std::cos(startAngle - magSweep * 0.5),
                 center.y + radius * std::sin(startAngle - magSweep * 0.5));
    sweepAngle = (midPos.dist(apex) <= midNeg.dist(apex)) ? magSweep : -magSweep;
    return true;
}

bool ShapeRecognizer::isArc(const std::vector<Point>& points, Point& start, Point& end, double& bulge)
{
    if (points.size() < 8)
        return false;

    start = points.front();
    end = points.back();
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double chordLen = std::hypot(dx, dy);
    if (chordLen < 18.0)
        return false;

    double nx = -dy / chordLen;
    double ny = dx / chordLen;
    double ux = dx / chordLen;
    double uy = dy / chordLen;

    double maxPos = 0.0, minNeg = 0.0;
    double pathLen = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        if (i > 0)
            pathLen += points[i].dist(points[i - 1]);
        double rx = points[i].x - start.x;
        double ry = points[i].y - start.y;
        double proj = rx * ux + ry * uy;
        if (proj < -0.35 * chordLen || proj > 1.35 * chordLen)
            return false;
        double perp = rx * nx + ry * ny;
        if (perp > maxPos) maxPos = perp;
        if (perp < minNeg) minNeg = perp;
    }

    double s = (std::abs(maxPos) >= std::abs(minNeg)) ? maxPos : minNeg;
    double opp = (s >= 0.0) ? std::abs(minNeg) : maxPos;
    if (std::abs(s) < 1e-6 || opp > 0.15 * std::abs(s))
        return false;

    bulge = s / chordLen;
    if (std::abs(bulge) < 0.14 || std::abs(bulge) > 1.35)
        return false;

    Point center;
    double radius = 0.0, startAngle = 0.0, sweepAngle = 0.0;
    if (!calcArcGeometry(start, end, bulge, center, radius, startAngle, sweepAngle))
        return false;

    double absSweep = std::abs(sweepAngle);
    double sweepDeg = absSweep * 180.0 / M_PI;
    if (radius < 10.0 || sweepDeg < 30.0 || sweepDeg > 285.0)
        return false;

    double arcLen = radius * absSweep;
    if (pathLen < 0.80 * arcLen || pathLen > 1.35 * arcLen)
        return false;

    double sumSq = 0.0, maxErr = 0.0;
    double sweepSign = (sweepAngle >= 0.0) ? 1.0 : -1.0;
    double prevUnwrapped = 0.0;
    double maxProg = 0.0;

    for (size_t i = 0; i < points.size(); ++i) {
        double d = points[i].dist(center);
        double err = std::abs(d - radius);
        sumSq += err * err;
        maxErr = std::max(maxErr, err);

        double ang = std::atan2(points[i].y - center.y, points[i].x - center.x);
        double rel = (ang - startAngle) * sweepSign;
        if (i > 0) {
            while (rel - prevUnwrapped > M_PI) rel -= 2.0 * M_PI;
            while (rel - prevUnwrapped < -M_PI) rel += 2.0 * M_PI;
        }
        prevUnwrapped = rel;
        if (rel < -0.20 * absSweep || rel > absSweep + 0.20 * absSweep)
            return false;
        if (maxProg - rel > 0.20 * absSweep)
            return false;
        maxProg = std::max(maxProg, rel);
    }

    double rmse = std::sqrt(sumSq / points.size());
    return rmse < std::max(3.0, 0.08 * radius) && maxErr < std::max(7.0, 0.16 * radius);
}

bool ShapeRecognizer::isCurvedArrow(const std::vector<Point>& points, Point& start, Point& end, double& bulge)
{
    if (points.size() < 10)
        return false;

    size_t bestTipIdx = 0;
    double bestScore = -1.0;
    size_t minIdx = std::max<size_t>(6, points.size() * 45 / 100);
    for (size_t i = minIdx; i + 2 < points.size(); ++i) {
        size_t prevIdx = (i >= 4) ? (i - 4) : 0;
        while (prevIdx > 0 && points[i].dist(points[prevIdx]) < 8.0)
            --prevIdx;
        double inDx = points[i].x - points[prevIdx].x;
        double inDy = points[i].y - points[prevIdx].y;
        double inLen = std::hypot(inDx, inDy);
        double outDx = points.back().x - points[i].x;
        double outDy = points.back().y - points[i].y;
        double outLen = std::hypot(outDx, outDy);
        if (inLen < 8.0 || outLen < 6.0)
            continue;
        double angDeg = std::acos(clampUnit((inDx * outDx + inDy * outDy) / (inLen * outLen))) * 180.0 / M_PI;
        if (angDeg >= 110.0 && angDeg > bestScore) {
            bestScore = angDeg;
            bestTipIdx = i;
        }
    }

    if (bestTipIdx < 7 || points.size() - bestTipIdx < 3)
        return false;

    std::vector<Point> shaft(points.begin(), points.begin() + bestTipIdx + 1);
    Point candStart, candEnd;
    double candBulge = 0.0;
    if (!isArc(shaft, candStart, candEnd, candBulge))
        return false;

    Point center;
    double radius = 0.0, startAngle = 0.0, sweepAngle = 0.0;
    if (!calcArcGeometry(candStart, candEnd, candBulge, center, radius, startAngle, sweepAngle))
        return false;

    double arcLen = radius * std::abs(sweepAngle);
    double rx = (candEnd.x - center.x) / radius;
    double ry = (candEnd.y - center.y) / radius;
    double ux = (sweepAngle >= 0.0) ? -ry : ry;
    double uy = (sweepAngle >= 0.0) ? rx : -rx;

    double headDx = points.back().x - candEnd.x;
    double headDy = points.back().y - candEnd.y;
    double headLen = std::hypot(headDx, headDy);
    if (headLen < std::max(8.0, 0.08 * arcLen) || headLen > 0.55 * arcLen)
        return false;

    double headAngle = std::acos(clampUnit((ux * headDx + uy * headDy) / headLen)) * 180.0 / M_PI;
    if (headAngle < 115.0 || headAngle > 170.0)
        return false;

    start = candStart;
    end = candEnd;
    bulge = candBulge;
    return true;
}
