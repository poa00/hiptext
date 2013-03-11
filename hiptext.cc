// hiptext - Image to Text Converter
// By Justine Tunney

#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "charquantizer.h"
#include "font.h"
#include "graphic.h"
#include "jpeg.h"
#include "macterm.h"
#include "movie.h"
#include "pixel.h"
#include "png.h"
#include "unicode.h"
#include "xterm256.h"
#include "termprinter.h"

using std::cout;
using std::string;
using std::wstring;

DEFINE_string(chars, u8"\u00a0\u2591\u2592\u2593\u2588",
              "The quantization character array");
DEFINE_bool(color, true, "Use --nocolor to disable color altogether");
DEFINE_bool(macterm, false, "Optimize for Mac OS X Terminal.app");
DEFINE_bool(xterm256, true, "Enable xterm-256color output");
DEFINE_bool(xterm256unicode, false, "Enable xterm256 double-pixel hack");
DEFINE_string(bg, "black", "The native background of your terminal specified "
              "as a CSS or X11 color value. If you're a real hacker this will "
              "be black, but some insane desktops like to coerce people into "
              "using white (or even purple!) terminal backgrounds by default. "
              "When using the --nocolor mode you should set this to white if "
              "you plan copy/pasting the output into something with a white "
              "background like if you were spamming Reddit");
DEFINE_bool(bgprint, false, "Enable explicit styling when printing characters "
            "that are nearly identical to the native terminal background");
DEFINE_string(space, u8"\u00a0", "The empty character to use when printing. "
              "By default this is a utf8 non-breaking space");
DEFINE_bool(stepthrough, false, "Whether to wait for human to press Return "
            "between frames. Only applicablae to movie playbacks");
DEFINE_int32(width, 0, "Width of rendering. Defaults to 0, in which case it "
           "automatically detects the terminal width. If height is not "
           "provided, it still maintains the aspect ratio. Cannot exceed the "
           "terminal width");
DEFINE_int32(height, 0, "Height of rendering. Defaults to 0, in which case it "
           "automatically maintains the aspect ratio with respect to width");
DEFINE_bool(equalize, false, "Use the histogram equalizer filter. You should "
            "use this when your image looks 'washed out' or grey when rendered "
            "in hiptext");
DEFINE_bool(spectrum, false, "Show color spectrum graph");

static const wchar_t kUpperHalfBlock = L'\u2580';
static const wchar_t kLowerHalfBlock = L'\u2584';
static const wchar_t kFullBlock = L'\u2588';

static int g_width;
static int g_cursor_saved;

void PrintImageXterm256(std::ostream& os, const Graphic& graphic) {
  TermPrinter out(os);
  Pixel bg = Pixel(FLAGS_bg);
  int bg256 = rgb_to_xterm256(bg);
  LOG(INFO) << "THE THING " << graphic.width() << "x" << graphic.height();
  for (int y = 0; y < graphic.height(); ++y) {
    for (int x = 0; x < graphic.width(); ++x) {
      int code = rgb_to_xterm256(graphic.Get(x, y).Copy().Opacify(bg));
      if (!FLAGS_bgprint && code == bg256) {
        out.SetBackground256(0);
      } else {
        out.SetBackground256(code);
      }
      out << FLAGS_space;
    }
    out.Reset();
    out << "\n";
  }
}

void PrintImageXterm256Unicode(std::ostream& os, const Graphic& graphic) {
  TermPrinter out(os);
  int height = graphic.height() - graphic.height() % 2;
  for (int y = 0; y < height; y += 2) {
    for (int x = 0; x < graphic.width(); ++x) {
      const Pixel& top = graphic.Get(x, y);
      const Pixel& bottom = graphic.Get(x, y + 1);
      int top256 = rgb_to_xterm256(top);
      int bottom256 = rgb_to_xterm256(bottom);
      out.SetForeground256(top256);
      out.SetBackground256(bottom256);
      out << kUpperHalfBlock;
    }
    out.Reset();
    out << "\n";
  }
}

void PrintImageMacterm(std::ostream& os, const Graphic& graphic) {
  TermPrinter out(os);
  Pixel bg = Pixel(FLAGS_bg);
  int height = graphic.height() - graphic.height() % 2;
  for (int y = 0; y < height; y += 2) {
    for (int x = 0; x < graphic.width(); ++x) {
      MactermColor color(graphic.Get(x, y + 0).Copy().Opacify(bg),
                         graphic.Get(x, y + 1).Copy().Opacify(bg));
      out.SetForeground256(color.fg());
      out.SetBackground256(color.bg());
      out << color.symbol();
    }
    out.Reset();
    out << "\n";
  }
}

void PrintImageNoColor(std::ostream& os, const Graphic& graphic) {
  Pixel bg = Pixel(FLAGS_bg);
  wstring chars = DecodeText(FLAGS_chars);
  CharQuantizer quantizer(chars, 256);
  for (int y = 0; y < graphic.height(); ++y) {
    for (int x = 0; x < graphic.width(); ++x) {
      const Pixel& pixel = graphic.Get(x, y);
      if (bg == Pixel::kWhite) {
        os << quantizer.Quantize(255 - static_cast<int>(pixel.grey() * 255));
      } else {
        os << quantizer.Quantize(static_cast<int>(pixel.grey() * 255));
      }
    }
    cout << "\n";
  }
}

static int AspectHeight(double new_width, double width, double height) {
  return new_width / width * height;
}

void PrintImage(std::ostream& os, Graphic graphic) {
  // Default to aspect-ratio unless |height| gflag is provided.
  int width = g_width;
  int height = (FLAGS_height
                ? FLAGS_height
                : AspectHeight(width, graphic.width(), graphic.height()));
  if (FLAGS_equalize) {
    // graphic.ToYUV();
    graphic.Equalize();
    // graphic.FromYUV();
  }
  if (FLAGS_color) {
    if (FLAGS_xterm256unicode) {
      PrintImageXterm256Unicode(os, graphic.BilinearScale(width, height));
    } else if (FLAGS_macterm) {
      PrintImageMacterm(os, graphic.BilinearScale(width, height));
    } else {
      PrintImageXterm256(os, graphic.BilinearScale(width, height / 2));
    }
  } else {
    PrintImageNoColor(os, graphic.BilinearScale(width, height / 2));
  }
}

void Sleep(int ms) {
  timespec req = {ms / 1000, ms % 1000 * 1000000};
  nanosleep(&req, nullptr);
}

winsize GetTerminalSize() {
  winsize ws;
  PCHECK(ioctl(0, TIOCGWINSZ, &ws) == 0);
  return ws;
}

void HideCursor() {
  cout << "\x1b[s";     // ANSI save cursor position.
  cout << "\x1b[?25l";  // ANSI make cursor invisible.
  g_cursor_saved = true;
}

void ShowCursor() {
  g_cursor_saved = false;
  cout << "\x1b[u";     // ANSI restore cursor position.
  cout << "\x1b[?25h";  // ANSI make cursor visible.
}

void ResetCursor() {
  cout << "\x1b[H";     // ANSI put cursor in top left.
}

void PrintMovie(Movie movie) {
  HideCursor();
  for (auto graphic : movie) {
    ResetCursor();
    PrintImage(cout, std::move(graphic));
    if (FLAGS_stepthrough) {
      string lol;
      std::getline(std::cin, lol);
    }
  }
  ShowCursor();
}

Graphic GenerateSpectrum(int width, int height) {
  int bar_width = static_cast<double>(width) * 0.05;
  int spec_width = width - bar_width * 4;
  double hh = static_cast<double>(height) / 2.0;
  Graphic res(width, height);
  for (int y = 0; y < height; ++y) {
    double fy = static_cast<double>(y);

    // Render the large color spectrum.
    for (int x = 0; x < spec_width; ++x) {
      double fx = static_cast<double>(x);
      res.Get(x, y) = Pixel(
          fx / spec_width,
          (y > hh) ? 1.0 : fy / (hh),
          (y < hh) ? 1.0 : 1.0 - (fy - (hh)) / (hh)).FromHSV();
    }

    // Render the grey bar.
    int offset = spec_width;
    for (int x = 0; x < bar_width; ++x) {
      res.Get(x + offset, y) =
          Pixel(0.0, 0.0, fy / height).FromHSV();
    }

    // Render the red/white gradient bar.
    offset += bar_width;
    for (int x = 0; x < bar_width; ++x) {
      res.Get(x + offset, y) = Pixel(1.0, fy / height, fy / height);
    }

    // Render the green/white gradient bar.
    offset += bar_width;
    for (int x = 0; x < bar_width; ++x) {
      res.Get(x + offset, y) = Pixel(fy / height, 1.0, fy / height);
    }

    // Render the blue/white gradient bar.
    offset += bar_width;
    for (int x = 0; x < bar_width; ++x) {
      res.Get(x + offset, y) = Pixel(fy / height, fy / height, 1.0);
    }
  }
  return res;
}

inline string GetExtension(const string& path) {
  string s = path.substr(path.find_last_of('.') + 1);
  std::transform(s.begin(), s.end(), s.begin(), tolower);
  return s;
}

void OnCtrlC(int /*signal*/) {
  if (g_cursor_saved) {
    ShowCursor();
  }
  exit(0);
}

int main(int argc, char** argv) {
  // if (!isatty(1))
  //   FLAGS_color = false;
  google::SetUsageMessage("hiptext [FLAGS]");
  google::SetVersionString("0.1");
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  const char* lang = std::getenv("LANG");
  if (lang == nullptr) lang = "en_US.utf8";
  std::locale::global(std::locale(lang));
  signal(SIGINT, OnCtrlC);
  InitFont();
  Movie::InitializeMain();

  // Calculate output dimensions according to the terminal.
  int term_width = GetTerminalSize().ws_col;
  g_width = std::min(term_width, FLAGS_width ? FLAGS_width : term_width);

  // Did they specify an option that requires no args?
  if (FLAGS_spectrum) {
    PrintImage(cout, GenerateSpectrum(GetTerminalSize().ws_col,
                                      GetTerminalSize().ws_row * 2 - 2));
    exit(0);
  }

  // Otherwise get an arg.
  if (argc < 2) {
    fprintf(stderr, "Missing file argument.\n"
            "Usage: %s [OPTIONS] [IMAGE_FILE | MOVIE_FILE]\n"
            "       %s --help\n", argv[0], argv[0]);
    exit(1);
  }
  string path = argv[1];

  // Otherwise, print a single media file.
  string extension = GetExtension(path);
  LOG(INFO) << "Hiptexting: " << argv[1];
  LOG(INFO) << "File Type: " << extension;
  if (extension == "png") {
    PrintImage(cout, LoadPNG(path));
  } else if (extension == "jpg" || extension == "jpeg") {
    PrintImage(cout, LoadJPEG(path));
  } else if (extension == "mov" || extension == "mp4" || extension == "flv" ||
             extension == "avi" || extension == "mkv") {
    PrintMovie(Movie(path, g_width));
  } else {
    fprintf(stderr, "Unknown Filetype: %s\n", extension.data());
    exit(1);
  }

  exit(0);
}

// For Emacs:
// Local Variables:
// mode:c++
// indent-tabs-mode:nil
// tab-width:2
// c-basic-offset:2
// c-file-style: nil
// End:
// For VIM:
// vim:set expandtab softtabstop=2 shiftwidth=2 tabstop=2:
