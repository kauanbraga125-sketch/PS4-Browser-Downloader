#include <SDL2/SDL.h>
#include "session.hpp"
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {
constexpr int SW=1920,SH=1080;
constexpr const char* TORRENT_DIR="/data/pkg/torrents";
constexpr const char* DOWNLOAD_DIR="/data/pkg/downloads";
struct Job {
 std::string path,name,error; std::unique_ptr<bt::Session> session; std::thread worker;
 std::mutex lock; bt::ProgressInfo progress{}; bool running=false,complete=false;
};
std::vector<std::unique_ptr<Job>> jobs; int selected=0;
std::string status="Copy .torrent files to /data/pkg/torrents";
SDL_Window* win=nullptr; SDL_Renderer* ren=nullptr;
void box(int x,int y,int w,int h,SDL_Color c){SDL_Rect r{x,y,w,h};SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,255);SDL_RenderFillRect(ren,&r);}
void text(const std::string&s,int x,int y,int scale,SDL_Color c){
 // Compact 5x7 font; lowercase is normalized to uppercase.
 static const unsigned char f[36][5]={
 {62,65,65,65,62},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},{127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},{0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},{127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},{62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},{63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},{7,8,112,8,7},{97,81,73,69,67},
 {62,69,73,81,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},{24,20,18,127,16},{47,73,73,73,49},{62,73,73,73,50},{1,113,9,5,3},{54,73,73,73,54},{38,73,73,73,62}};
 SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,255);
 for(char ch:s){ch=static_cast<char>(toupper(static_cast<unsigned char>(ch)));int idx=ch>='A'&&ch<='Z'?ch-'A':(ch>='0'&&ch<='9'?26+ch-'0':-1);
  if(idx>=0)for(int a=0;a<5;a++)for(int b=0;b<7;b++)if(f[idx][a]&(1<<b)){SDL_Rect p{x+a*scale,y+b*scale,scale,scale};SDL_RenderFillRect(ren,&p);}
  x+=6*scale;
 }
}
std::string base(const std::string&p){auto n=p.find_last_of('/');return n==std::string::npos?p:p.substr(n+1);}
void scan(){
 DIR*d=opendir(TORRENT_DIR);if(!d){status="Torrent folder unavailable";return;}
 std::vector<std::string> found;dirent*e;
 while((e=readdir(d))){std::string n=e->d_name;if(n.size()>=8){std::string ext=n.substr(n.size()-8);std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);if(ext==".torrent")found.push_back(std::string(TORRENT_DIR)+"/"+n);}}
 closedir(d);std::sort(found.begin(),found.end());
 for(const auto&p:found){bool seen=false;for(const auto&j:jobs)if(j->path==p)seen=true;if(!seen){auto j=std::make_unique<Job>();j->path=p;j->name=base(p);jobs.push_back(std::move(j));}}
 if(selected>=static_cast<int>(jobs.size()))selected=std::max(0,static_cast<int>(jobs.size())-1);
 status="Library refreshed";
}
void start(){
 if(jobs.empty()){status="No torrent files found";return;}Job&j=*jobs[selected];
 if(j.running||j.complete){status="Already active or complete";return;}
 try{j.session=std::make_unique<bt::Session>(j.path,DOWNLOAD_DIR);}catch(const std::exception&e){j.error=e.what();status="Torrent parse failed";return;}
 j.session->set_progress_callback([p=&j](const bt::ProgressInfo&i){std::lock_guard<std::mutex>g(p->lock);p->progress=i;});
 j.running=true;j.error.clear();
 j.worker=std::thread([p=&j](){try{p->session->start();p->complete=p->session->is_complete();}catch(const std::exception&e){p->error=e.what();}p->running=false;});
 status="Starting torrent";
}
void stop(){if(jobs.empty())return;Job&j=*jobs[selected];if(j.session&&j.running){status="Stop requested";j.session->stop();}else status="Torrent is not running";}
void render(){
 box(0,0,SW,SH,{10,16,28});box(0,0,SW,120,{20,65,112});
 text("PS4 TORRENT",64,38,6,{245,248,255});text("BITTORRENT CLIENT",700,55,3,{160,205,245});
 text("TORRENT LIBRARY",80,170,4,{90,190,255});text("UP DOWN SELECT   X START   O STOP   SQUARE REFRESH",80,235,2,{210,220,235});
 int y=310;
 for(int i=0;i<(int)jobs.size()&&i<7;i++,y+=82){bool sel=i==selected;box(64,y,1792,66,sel?SDL_Color{30,75,118}:SDL_Color{22,32,48});text(jobs[i]->name.substr(0,35),92,y+22,3,{245,245,250});
  const char*st=jobs[i]->complete?"DONE":jobs[i]->running?"ACTIVE":jobs[i]->error.size()?"ERROR":"READY";text(st,1300,y+24,2,{150,205,225});
  if(jobs[i]->session){bt::ProgressInfo p{};{std::lock_guard<std::mutex>g(jobs[i]->lock);p=jobs[i]->progress;}box(1510,y+26,270,12,{55,65,80});box(1510,y+26,(int)(270*std::max(0.f,std::min(100.f,p.pct))/100.f),12,{40,190,125});}
 }
 box(64,920,1792,90,{20,31,47});text(status.substr(0,90),88,950,2,{230,235,245});text("DOWNLOAD LOCATION  /DATA/PKG/DOWNLOADS",80,1040,2,{125,155,190});
 SDL_RenderPresent(ren);
}
}
int main(){
 setvbuf(stdout,nullptr,_IONBF,0);mkdir(TORRENT_DIR,0777);mkdir(DOWNLOAD_DIR,0777);
 if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK)!=0)return 1;
 win=SDL_CreateWindow("PS4 Torrent",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,SW,SH,0);if(!win){SDL_Quit();return 2;}
 ren=SDL_CreateSoftwareRenderer(SDL_GetWindowSurface(win));if(!ren){SDL_DestroyWindow(win);SDL_Quit();return 3;}
 if(SDL_NumJoysticks()>0)SDL_JoystickOpen(0);scan();bool alive=true;
 while(alive){SDL_Event e;while(SDL_PollEvent(&e)){
  if(e.type==SDL_QUIT)alive=false;
  if(e.type==SDL_JOYBUTTONDOWN){if(e.jbutton.button==0)start();else if(e.jbutton.button==1)stop();else if(e.jbutton.button==2)scan();else if(e.jbutton.button==6)alive=false;else if(e.jbutton.button==11&&!jobs.empty())selected=(selected+(int)jobs.size()-1)%jobs.size();else if(e.jbutton.button==12&&!jobs.empty())selected=(selected+1)%jobs.size();}
  if(e.type==SDL_JOYHATMOTION){if(e.jhat.value&SDL_HAT_UP&&!jobs.empty())selected=(selected+(int)jobs.size()-1)%jobs.size();if(e.jhat.value&SDL_HAT_DOWN&&!jobs.empty())selected=(selected+1)%jobs.size();}
  if(e.type==SDL_KEYDOWN){auto k=e.key.keysym.sym;if(k==SDLK_UP&&!jobs.empty())selected=(selected+(int)jobs.size()-1)%jobs.size();else if(k==SDLK_DOWN&&!jobs.empty())selected=(selected+1)%jobs.size();else if(k==SDLK_RETURN)start();else if(k==SDLK_BACKSPACE)stop();else if(k==SDLK_r)scan();else if(k==SDLK_ESCAPE)alive=false;}
 }render();SDL_UpdateWindowSurface(win);SDL_Delay(33);}
 for(auto&j:jobs)if(j->session&&j->running)j->session->stop();for(auto&j:jobs)if(j->worker.joinable())j->worker.join();
 SDL_DestroyRenderer(ren);SDL_DestroyWindow(win);SDL_Quit();return 0;
}