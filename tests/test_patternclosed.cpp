// Pattern on path with a spacing on a closed 25 x 25 square (perimeter 100):
// 5 items at 25 mm: no copy may land on the original.
#include "../src/element.h"
#include "../src/vectorops.h"
#include <QGuiApplication>
#include <cstdio>
using namespace c2d;
int main(int argc,char**argv){
  QGuiApplication app(argc,argv);
  QJsonObject L;
  Element path=Element::makeRectangle({50,50},25,25,L);
  const QVector<QTransform> t=vec::pathTransforms(path.painterPath,5,25.0,false);
  printf("%d placements\n",(int)t.size());
  for(const QTransform&x:t) printf("  dx %.4f dy %.4f\n",x.dx(),x.dy());
  int stacked=0; for(const QTransform&x:t) if(std::hypot(x.dx(),x.dy())<1e-6) ++stacked;
  printf("copies on top of the original: %d\n",stacked);
  // circle of circumference 100 too
  Element circ=Element::makeCircle({50,50},100/(2*M_PI),L);
  const QVector<QTransform> t2=vec::pathTransforms(circ.painterPath,5,25.0,false);
  printf("circle: %d placements, last dx %.4f dy %.4f\n",(int)t2.size(),t2.isEmpty()?0:t2.last().dx(),t2.isEmpty()?0:t2.last().dy());
  int nearStart=0; for(const QTransform&x:t2) if(std::hypot(x.dx(),x.dy())<0.05) ++nearStart;
  // A figure-eight passing through its start halfway: the copy that lands
  // there is skipped, the second lobe still gets its copy.
  Element eight=Element::makePath({{0,0},{10,5},{10,-5},{0,0},{-10,5},{-10,-5}},true,L);
  const QVector<QTransform> t3=vec::pathTransforms(eight.painterPath,4,0.0,false);
  printf("figure-eight: %d placements\n",(int)t3.size());
  // A 1 mm circle with 80 copies: the first ones are near the start, the rest are not.
  Element tiny=Element::makeCircle({50,50},0.5,L);
  const QVector<QTransform> t4=vec::pathTransforms(tiny.painterPath,80,0.0,false);
  printf("1 mm circle, 80: %d placements\n",(int)t4.size());
  return stacked||nearStart||t3.size()!=2||t4.size()<40?1:0;
}
