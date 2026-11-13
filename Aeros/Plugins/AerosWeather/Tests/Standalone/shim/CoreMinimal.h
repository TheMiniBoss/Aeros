// Minimal stand-in for the Unreal headers used by the Aeros simulation core.
// Lets Core/AerosSimulation.cpp and Core/AerosSnapshot.cpp compile with a plain C++20
// compiler (no engine), to test the physics quickly. Test-only: never part of the UE build.
// ParallelFor runs sequentially here (same results: the core is deterministic).
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <memory>
#include <limits>
#include <chrono>
#include <functional>
typedef int32_t int32; typedef uint32_t uint32; typedef uint8_t uint8; typedef int64_t int64; typedef uint64_t uint64;
#define FORCEINLINE inline
#define USTRUCT(...)
#define UPROPERTY(...)
#define UENUM(...)
#define UMETA(...)
#define GENERATED_BODY()
#define AEROSWEATHER_API
#undef PI
#define PI 3.14159265358979f
enum class EAllowShrinking { No, Yes };
namespace ESPMode { enum Type { ThreadSafe }; }
template<typename T, ESPMode::Type M = ESPMode::ThreadSafe> using TSharedPtr = std::shared_ptr<T>;
template<typename T> struct TNumericLimits { static T Max(){return std::numeric_limits<T>::max();} };
struct FMath {
  template<class T> static T Clamp(T v, T a, T b){ return v<a?a:(v>b?b:v);}
  template<class T> static T Max(T a, T b){return a>b?a:b;}
  template<class T> static T Min(T a, T b){return a<b?a:b;}
  template<class T> static T Abs(T a){return a<0?-a:a;}
  template<class T> static T Square(T a){return a*a;}
  static float Sqrt(float a){return std::sqrt(a);} static float Exp(float a){return std::exp(a);}
  static float Pow(float a,float b){return std::pow(a,b);} static float Sin(float a){return std::sin(a);} static float Cos(float a){return std::cos(a);}
  static float Asin(float a){return std::asin(a);} static float Atan2(float a,float b){return std::atan2(a,b);}
  static float Loge(float a){return std::log(a);} static float LogX(float b,float a){return std::log(a)/std::log(b);}
  static int32 FloorToInt(float a){return (int32)std::floor(a);} static int64 FloorToInt64(double a){return (int64)std::floor(a);}
  static int32 CeilToInt(float a){return (int32)std::ceil(a);} static int32 RoundToInt(float a){return (int32)std::lround(a);}
  static bool IsFinite(float x){return std::isfinite(x);} static bool IsNearlyEqual(float a,float b,float t){return std::fabs(a-b)<=t;} static float Fmod(float a,float b){return std::fmod(a,b);} static double Fmod(double a,double b){return std::fmod(a,b);}
  static float DegreesToRadians(float d){return d*PI/180.f;} static float RadiansToDegrees(float r){return r*180.f/PI;}
  template<class T> static T Lerp(const T& a,const T& b,float t){return a+(b-a)*t;}
};
struct FVector { double X=0,Y=0,Z=0; FVector(){} FVector(double x,double y,double z):X(x),Y(y),Z(z){}
  static const FVector ZeroVector, UpVector; double Size() const {return std::sqrt(X*X+Y*Y+Z*Z);}
  static double Dist2D(const FVector&a,const FVector&b){return std::hypot(a.X-b.X,a.Y-b.Y);}
  double Size2D() const {return std::hypot(X,Y);} bool IsZero() const {return X==0&&Y==0&&Z==0;}
  FVector GetSafeNormal2D() const {double l=Size2D(); return l>1e-8?FVector(X/l,Y/l,0):FVector(0,0,0);}
  bool Equals(const FVector&o,double t=1e-4) const {return std::fabs(X-o.X)<=t&&std::fabs(Y-o.Y)<=t&&std::fabs(Z-o.Z)<=t;} };
inline const FVector FVector::ZeroVector(0,0,0); inline const FVector FVector::UpVector(0,0,1);
struct FVector2D { double X=0,Y=0; FVector2D(){} FVector2D(double x,double y):X(x),Y(y){}
  static const FVector2D ZeroVector; FVector2D operator-(const FVector2D&o)const{return {X-o.X,Y-o.Y};} FVector2D operator+(const FVector2D&o)const{return {X+o.X,Y+o.Y};}
  FVector2D operator*(double s)const{return {X*s,Y*s};} double Size()const{return std::hypot(X,Y);} FVector2D GetSafeNormal()const{double l=Size(); return l>1e-8?FVector2D(X/l,Y/l):FVector2D(0,0);} static double Distance(const FVector2D&a,const FVector2D&b){return std::hypot(a.X-b.X,a.Y-b.Y);} };
inline const FVector2D FVector2D::ZeroVector(0,0);
struct FIntPoint { int32 X=0,Y=0; FIntPoint(){} FIntPoint(int32 x,int32 y):X(x),Y(y){} };
template<typename T> class TArray { std::vector<T> v; public:
  int32 Num() const {return (int32)v.size();} void SetNum(int32 n){v.resize(n);} void SetNumUninitialized(int32 n){v.resize(n);} void SetNumZeroed(int32 n){v.assign(n,T());} void Init(const T& x,int32 n){v.assign(n,x);}
  T& operator[](int32 i){return v[i];} const T& operator[](int32 i)const{return v[i];} T* GetData(){return v.data();} const T* GetData()const{return v.data();}
  void Reset(int32 n=0){v.clear(); v.reserve(n);} int32 Add(const T& x){v.push_back(x); return Num()-1;} T& AddDefaulted_GetRef(){v.emplace_back(); return v.back();}
  T Pop(EAllowShrinking=EAllowShrinking::Yes){T x=v.back(); v.pop_back(); return x;} void RemoveAt(int32 i,int32 c=1){v.erase(v.begin()+i,v.begin()+i+c);}
  template<class P> int32 RemoveAll(P p){auto it=std::remove_if(v.begin(),v.end(),p); int32 n=(int32)(v.end()-it); v.erase(it,v.end()); return n;}
  bool IsValidIndex(int32 i)const{return i>=0&&i<Num();} template<class P> void Sort(P p){std::sort(v.begin(),v.end(),p);}
  auto begin(){return v.begin();} auto end(){return v.end();} auto begin()const{return v.begin();} auto end()const{return v.end();} };
inline void ParallelFor(int32 n, const std::function<void(int32)>& f){ for(int32 i=0;i<n;++i) f(i); }
struct FPlatformTime { static double Seconds(){ return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); } };
template<class T> decltype(auto) MoveTemp(T&& x){return std::move(x);}
inline FVector operator-(const FVector&a,const FVector&b){return {a.X-b.X,a.Y-b.Y,a.Z-b.Z};}
inline FVector operator+(const FVector&a,const FVector&b){return {a.X+b.X,a.Y+b.Y,a.Z+b.Z};}
inline FVector operator*(const FVector&a,float s){return {a.X*s,a.Y*s,a.Z*s};}
struct FVector4f { float X=0,Y=0,Z=0,W=0; FVector4f(){} FVector4f(float x,float y,float z,float w):X(x),Y(y),Z(z),W(w){} };
