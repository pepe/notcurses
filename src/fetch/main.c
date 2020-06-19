#include <pwd.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <locale.h>
#include <semaphore.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/utsname.h>
#include <notcurses/notcurses.h>

typedef struct distro_info {
  const char* name;            // must match 'lsb_release -i'
  const char* logofile;        // kept at original aspect ratio, lain atop bg
} distro_info;

typedef struct fetched_info {
  char* username;              // we borrow a reference
  char hostname[_POSIX_HOST_NAME_MAX];
  const distro_info* distro;
  char* distro_pretty;
  char* kernel;                // strdup(uname(2)->name)
  char* kernver;               // strdup(uname(2)->version);
  char* desktop;               // getenv("XDG_CURRENT_DESKTOP")
  char* shell;                 // getenv("SHELL")
  char* term;                  // getenv("TERM")
  int dimy, dimx;              // extracted from xrandr
  char* cpu_model;             // FIXME don't handle hetero setups yet
  int core_count;
} fetched_info;

static int
fetch_env_vars(fetched_info* fi){
  fi->desktop = getenv("XDG_CURRENT_DESKTOP");
  fi->shell = getenv("SHELL");
  fi->term = getenv("TERM");
  return 0;
}

static distro_info distros[] = {
  {
    .name = "arch",
    // from core/filesystem
    .logofile = "/usr/share/pixmaps/archlinux.png",
  }, {
    .name = "debian",
    // from desktop-base package
    .logofile = "/usr/share/desktop-base/debian-logos/logo-text-256.png",
  }, {
    .name = "fedora",
    // from redhat-lsb-core package
    .logofile = "/usr/share/pixmaps/fedora-logo.png",
  }, {
    .name = NULL,
    .logofile = NULL,
  },
};

static int
fetch_cpu_info(fetched_info* fi){
  FILE* cpuinfo = fopen("/proc/cpuinfo", "re");
  char buf[BUFSIZ];
  while(fgets(buf, sizeof(buf), cpuinfo)){
#define TAG "model name\t:"
    if(strncmp(buf, TAG, strlen(TAG)) == 0){
      if(fi->cpu_model == NULL){
        char* nl = strchr(buf + strlen(TAG), '\n');
        *nl = '\0';
        fi->cpu_model = strdup(buf + strlen(TAG));
      }
      ++fi->core_count;
    }
#undef TAG
  }
  return 0;
}

static char*
pipe_getline(const char* cmdline){
  FILE* p = popen(cmdline, "re");
  if(p == NULL){
    fprintf(stderr, "Error running %s (%s)\n", cmdline, strerror(errno));
    return NULL;
  }
  char* buf = malloc(BUFSIZ); // gatesv("BUFSIZ bytes is enough for anyone")
  if(fgets(buf, BUFSIZ, p) == NULL){
    fprintf(stderr, "Error reading from %s (%s)\n", cmdline, strerror(errno));
    fclose(p);
    free(buf);
    return NULL;
  }
  // FIXME read any remaining junk so as to stave off SIGPIPEs?
  if(fclose(p)){
    fprintf(stderr, "Error closing pipe (%s)\n", strerror(errno));
    free(buf);
    return NULL;
  }
  return buf;
}

static int
fetch_x_props(fetched_info* fi){
  char* xrandr = pipe_getline("xrandr --current");
  if(xrandr == NULL){
    return -1;
  }
  char* randrcurrent = strstr(xrandr, " current ");
  if(randrcurrent == NULL){
    free(xrandr);
    return -1;
  }
  randrcurrent += strlen(" current ");
  if(sscanf(randrcurrent, "%d x %d", &fi->dimx, &fi->dimy) != 2){
    free(xrandr);
    return -1;
  }
  return 0;
}

static const distro_info*
linux_ncneofetch(fetched_info* fi){
  FILE* osinfo = fopen("/etc/os-release", "re");
  if(osinfo == NULL){
    return NULL;
  }
  char buf[BUFSIZ];
  char* distro = NULL;
  while(fgets(buf, sizeof(buf), osinfo)){
#define PRETTY "PRETTY_NAME=\""
#define ID "ID=" // no quotes on this one
    if(strncmp(buf, ID, strlen(ID)) == 0){
      char* nl = strchr(buf + strlen(ID), '\n');
      if(nl){
        *nl = '\0';
        distro = buf + strlen(ID);
        break;
      }
    }else if(!fi->distro_pretty && strncmp(buf, PRETTY, strlen(PRETTY)) == 0){
      char* nl = strchr(buf + strlen(PRETTY), '"');
      if(nl){
        *nl = '\0';
        fi->distro_pretty = strdup(buf + strlen(PRETTY));
      }
    }
  }
#undef ID
#undef PRETTY
  fclose(osinfo);
  if(distro == NULL){
    return NULL;
  }
  const distro_info* dinfo = NULL;
  for(dinfo = distros ; dinfo->name ; ++dinfo){
    if(strcmp(dinfo->name, distro) == 0){
      break;
    }
  }
  if(dinfo->name == NULL){
    dinfo = NULL;
  }
  return dinfo;
}

static int
unix_getusername(fetched_info* fi){
  if( (fi->username = getenv("LOGNAME")) ){
    if( (fi->username = strdup(fi->username)) ){
      return 0;
    }
  }
  uid_t uid = getuid();
  struct passwd* p = getpwuid(uid);
  if(p == NULL){
    return -1;
  }
  fi->username = strdup(p->pw_name);
  return 0;
}

static int
unix_gethostname(fetched_info* fi){
  if(gethostname(fi->hostname, sizeof(fi->hostname)) == 0){
    char* fqdn = strchr(fi->hostname, '.');
    if(fqdn){
      *fqdn = '\0';
    }
    return 0;
  }
  return -1;
}

typedef enum {
  NCNEO_LINUX,
  NCNEO_FREEBSD,
  NCNEO_UNKNOWN,
} ncneo_kernel_e;

static ncneo_kernel_e
get_kernel(fetched_info* fi){
  struct utsname uts;
  if(uname(&uts)){
    fprintf(stderr, "Failure invoking uname (%s)\n", strerror(errno));
    return -1;
  }
  fi->kernel = strdup(uts.sysname);
  fi->kernver = strdup(uts.release);
  if(strcmp(uts.sysname, "Linux") == 0){
    return NCNEO_LINUX;
  }else if(strcmp(uts.sysname, "FreeBSD") == 0){
    return NCNEO_FREEBSD;
  }
  fprintf(stderr, "Unknown operating system via uname: %s\n", uts.sysname);
  return NCNEO_UNKNOWN;
}

static struct ncplane*
display(struct notcurses* nc, const distro_info* dinfo){
  if(dinfo->logofile){
    int dimy, dimx;
    nc_err_e err;
    struct ncvisual* ncv = ncvisual_from_file(dinfo->logofile, &err);
    if(ncv == NULL){
      fprintf(stderr, "Error opening logo file at %s\n", dinfo->logofile);
      return NULL;
    }
    struct ncvisual_options vopts = {
      .scaling = NCSCALE_SCALE,
      .blitter = NCBLIT_2x2,
      .n = notcurses_stddim_yx(nc, &dimy, &dimx),
    };
    int y, x, scaley, scalex;
    ncvisual_geom(nc, ncv, &vopts, &y, &x, &scaley, &scalex);
    if(y / scaley < dimy){
      vopts.y = (dimy - (y + (scaley - 1)) / scaley) / 2;
    }
    if(x / scalex < dimx){
      vopts.x = (dimx - (x + (scalex - 1)) / scalex) / 2;
    }
    if(ncvisual_render(nc, ncv, &vopts) == NULL){
      ncvisual_destroy(ncv);
      return NULL;
    }
    ncvisual_destroy(ncv);
  }
  return 0;
}

static const distro_info*
freebsd_ncneofetch(fetched_info* fi){
  static const distro_info fbsd = {
    .name = "FreeBSD",
    .logofile = NULL, // FIXME
  };
  (void)fi; // FIXME set up distro_pretty
  return &fbsd;
}

static int
drawpalette(struct notcurses* nc){
  int psize = notcurses_palette_size(nc);
  if(psize > 256){
    psize = 256;
  }
  int dimy, dimx;
  struct ncplane* stdn = notcurses_stddim_yx(nc, &dimy, &dimx);
  if(dimx < 64){
    return -1;
  }
  cell c = CELL_SIMPLE_INITIALIZER(' ');
  // FIXME find a better place to put it
  const int yoff = 2;
  for(int y = yoff ; y < yoff + psize / 64 ; ++y){
    for(int x = (dimx - 64) / 2 ; x < dimx / 2 + 32 ; ++x){
      const int truex = x - (dimx - 64) / 2;
      if((y - yoff) * 64 + truex >= psize){
        break;
      }
      cell_set_bg_palindex(&c, (y - yoff) * 64 + truex);
      ncplane_putc_yx(stdn, y, x, &c);
    }
  }
  return 0;
}

static int
infoplane(struct notcurses* nc, const fetched_info* fi){
  // FIXME look for an area without background logo in it. pick the one
  // closest to the center horizontally, and lowest vertically. if none
  // can be found, just center it on the bottom as we do now
  const int dimy = ncplane_dim_y(notcurses_stdplane(nc));
  const int planeheight = 8;
  const int planewidth = 60;
  struct ncplane* infop = ncplane_aligned(notcurses_stdplane(nc),
                                          planeheight, planewidth,
                                          dimy - (planeheight + 1),
                                          NCALIGN_CENTER, NULL);
  if(infop == NULL){
    return -1;
  }
  ncplane_set_fg_rgb(infop, 0xd0, 0xd0, 0xd0);
  ncplane_set_attr(infop, NCSTYLE_UNDERLINE);
  ncplane_printf_aligned(infop, 1, NCALIGN_LEFT, " %s %s", fi->kernel, fi->kernver);
  ncplane_printf_aligned(infop, 1, NCALIGN_RIGHT, "%s ", fi->distro_pretty);
  ncplane_set_attr(infop, NCSTYLE_NONE);
#ifdef __linux__
  struct sysinfo sinfo;
  sysinfo(&sinfo);
  char totalmet[BPREFIXSTRLEN + 1], usedmet[BPREFIXSTRLEN + 1];
  bprefix(sinfo.totalram, 1, totalmet, 1);
  bprefix(sinfo.totalram - sinfo.freeram, 1, usedmet, 1);
  ncplane_printf_aligned(infop, 2, NCALIGN_LEFT, " RAM: %s/%s\n", usedmet, totalmet);
  ncplane_printf_aligned(infop, 2, NCALIGN_RIGHT, "Processes: %hu ", sinfo.procs);
#endif
  ncplane_printf_aligned(infop, 3, NCALIGN_LEFT, " DM: %s", fi->desktop);
  ncplane_printf_aligned(infop, 3, NCALIGN_RIGHT, "Shell: %s ", fi->shell);
  ncplane_printf_aligned(infop, 4, NCALIGN_LEFT, " TERM: %s", fi->term);
  ncplane_printf_aligned(infop, 4, NCALIGN_RIGHT, "Screen0: %dx%d ", fi->dimx, fi->dimy);
  ncplane_set_attr(infop, NCSTYLE_ITALIC);
  ncplane_printf_aligned(infop, 6, NCALIGN_CENTER, "%s (%d cores)", fi->cpu_model, fi->core_count);
  cell ul = CELL_TRIVIAL_INITIALIZER; cell ur = CELL_TRIVIAL_INITIALIZER;
  cell ll = CELL_TRIVIAL_INITIALIZER; cell lr = CELL_TRIVIAL_INITIALIZER;
  cell hl = CELL_TRIVIAL_INITIALIZER; cell vl = CELL_TRIVIAL_INITIALIZER;
  if(cells_rounded_box(infop, 0, 0, &ul, &ur, &ll, &lr, &hl, &vl)){
    return -1;
  }
  cell_set_fg_rgb(&ul, 0x90, 0x90, 0x90);
  cell_set_fg_rgb(&ur, 0x90, 0x90, 0x90);
  cell_set_fg_rgb(&ll, 0, 0, 0);
  cell_set_fg_rgb(&lr, 0, 0, 0);
  unsigned ctrlword = NCBOXGRAD_BOTTOM | NCBOXGRAD_LEFT | NCBOXGRAD_RIGHT;
  if(ncplane_perimeter(infop, &ul, &ur, &ll, &lr, &hl, &vl, ctrlword)){
    return -1;
  }
  ncplane_home(infop);
  uint64_t channels = 0;
  channels_set_fg_rgb(&channels, 0, 0xff, 0);
  ncplane_hline_interp(infop, &hl, planewidth / 2, ul.channels, channels);
  ncplane_hline_interp(infop, &hl, planewidth / 2, channels, ur.channels);
  cell_release(infop, &ul); cell_release(infop, &ur);
  cell_release(infop, &ll); cell_release(infop, &lr);
  cell_release(infop, &hl); cell_release(infop, &vl);
  ncplane_set_fg_rgb(infop, 0xff, 0xff, 0xff);
  ncplane_set_attr(infop, NCSTYLE_BOLD);
  if(ncplane_printf_aligned(infop, 0, NCALIGN_CENTER, "[ %s@%s ]",
                            fi->username, fi->hostname) < 0){
    return -1;
  }
  channels_set_fg_rgb(&channels, 0, 0, 0);
  channels_set_bg_rgb(&channels, 0x50, 0x50, 0x50);
  ncplane_set_base(infop, " ", 0, channels);
  return 0;
}

struct marshal {
  struct notcurses* nc;
  const distro_info* dinfo;
  sem_t sem;
};

static void*
display_thread(void* vmarshal){
  struct marshal* m = vmarshal;
  if(m->dinfo){
    display(m->nc, m->dinfo);
  }
  drawpalette(m->nc);
  sem_post(&m->sem);
  pthread_detach(pthread_self());
  return NULL;
}

static int
ncneofetch(struct notcurses* nc){
  fetched_info fi = {};
  ncneo_kernel_e kern = get_kernel(&fi);
  switch(kern){
    case NCNEO_LINUX:
      fi.distro = linux_ncneofetch(&fi);
      break;
    case NCNEO_FREEBSD:
      fi.distro = freebsd_ncneofetch(&fi);
      break;
    case NCNEO_UNKNOWN:
      break;
  }
  // go ahead and spin the image load + render into its own thread while the
  // rest of the fetching continues. cuts total runtime.
  struct marshal display_marshal = {
    .nc = nc,
    .dinfo = fi.distro,
  };
  sem_init(&display_marshal.sem, 0, 0);
  pthread_t tid;
  if(pthread_create(&tid, NULL, display_thread, &display_marshal)){
    sem_post(&display_marshal.sem);
  }
  unix_gethostname(&fi);
  unix_getusername(&fi);
  fetch_env_vars(&fi);
  fetch_x_props(&fi);
  fetch_cpu_info(&fi);
  sem_wait(&display_marshal.sem);
  if(infoplane(nc, &fi)){
    return -1;
  }
  if(notcurses_render(nc)){
    return -1;
  }
  return 0;
}

int main(void){
  if(setlocale(LC_ALL, "") == NULL){
    fprintf(stderr, "Warning: couldn't set locale based off LANG\n");
  }
  struct notcurses_options nopts = {
    .flags = NCOPTION_INHIBIT_SETLOCALE | NCOPTION_NO_ALTERNATE_SCREEN |
              NCOPTION_SUPPRESS_BANNERS,
  };
  struct notcurses* nc = notcurses_init(&nopts, NULL);
  if(nc == NULL){
    return EXIT_FAILURE;
  }
  int r = ncneofetch(nc);
  if(notcurses_stop(nc)){
    return EXIT_FAILURE;
  }
  return r ? EXIT_FAILURE : EXIT_SUCCESS;
}