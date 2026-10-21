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
  return stacked||nearStart?1:0;
}
