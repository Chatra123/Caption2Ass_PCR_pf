//------------------------------------------------------------------------------
// Caption2Ass_PCR.cpp : Defines the entry point for the console application.
//------------------------------------------------------------------------------

#include "stdafx.h"
#include <shlwapi.h>
#include <vector>


#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <time.h>
#include <chrono>
#include <thread>
using namespace std::chrono;
using namespace std;


#include "CommRoutine.h"
#include "CaptionDllUtil.h"
#include "cmdline.h"
#include "tslutil.h"
#include "Caption2Ass_PCR.h"

#define WRAP_AROUND_VALUE           (1LL << 33)
#define WRAP_AROUND_CHECK_VALUE     ((1LL << 32) - 1)
#define PCR_MAXIMUM_INTERVAL        (100)

static const unsigned char utf8_bom[3] = { 0xEF, 0xBB, 0xBF };

enum {
  C2A_SUCCESS = 0,
  C2A_FAILURE = 1,
  C2A_ERR_DLL = 2,
  C2A_ERR_PARAM = 3,
  C2A_ERR_MEMORY = 4
};

typedef struct _ASS_COLOR {
  unsigned char   ucR;
  unsigned char   ucG;
  unsigned char   ucB;
  unsigned char   ucAlpha;
} ASS_COLOR;

typedef struct _LINE_STR {
  ASS_COLOR       outCharColor;
  BOOL            outUnderLine;
  BOOL            outShadow;
  BOOL            outBold;
  BOOL            outItalic;
  BYTE            outFlushMode;
  std::string     str;
} LINE_STR, *PLINE_STR;

typedef std::vector<PLINE_STR> STRINGS_LIST;

typedef struct _CAPTION_LINE {
  UINT            index;
  DWORD           startTime;
  DWORD           endTime;
  BYTE            outCharSizeMode;
  WORD            outCharW;
  WORD            outCharH;
  WORD            outCharHInterval;
  WORD            outCharVInterval;
  WORD            outPosX;
  WORD            outPosY;
  BYTE            outHLC;     //must ignore low 4bits
  STRINGS_LIST    outStrings;
} CAPTION_LINE, *PCAPTION_LINE;

typedef std::vector<PCAPTION_LINE> CAPTION_LIST;

#define HMS(T, h, m, s, ms)             \
do {                                    \
    ms = (int)(T) % 1000;               \
    s  = (int)((T) / 1000) % 60;        \
    m  = (int)((T) / (1000 * 60)) % 60; \
    h  = (int)((T) / (1000 * 60 * 60)); \
} while (0)

typedef enum {
  C2A_PARAM_ALL,
  C2A_PARAM_PID,
  C2A_PARAM_CLI,
  C2A_PARAM_ASS,
  C2A_PARAM_INVALID
} c2a_parameter_type;

class ITimestampHandler
{
public:
  // Timestamp handlers
  long long       startPCR;
  long long       lastPCR;
  long long       lastPTS;
  long long       basePCR;
  long long       basePTS;
  long long       correctTS;

public:
  void init_timestamp(void)
  {
    this->startPCR = TIMESTAMP_INVALID_VALUE;
    this->lastPCR = TIMESTAMP_INVALID_VALUE;
    this->lastPTS = TIMESTAMP_INVALID_VALUE;
    this->basePCR = 0;
    this->basePTS = 0;
    this->correctTS = 0;
  }
  virtual void initialize(void)
  {
    this->init_timestamp();
  }
};

class IAppHandler : public ITimestampHandler
{
public:
  // Control informations
  BOOL            bCreateOutput;
  BOOL            bUnicode;
  int             sidebar_size;   // ASS only
  size_t          string_length;

protected:
  // Parameter handlers
  CCaption2AssParameter  *param;

protected:
  void initialize(void)
  {
    this->init_timestamp();
    this->bCreateOutput = FALSE;
    this->bUnicode = FALSE;
    this->sidebar_size = 0;
    this->string_length = 0;
    this->param = NULL;
  }

public:
  void *GetParam(c2a_parameter_type param_type)
  {
    void *param = NULL;
    switch (param_type) {
    case C2A_PARAM_ALL:
      param = static_cast<void *>(this->param);
      break;
    case C2A_PARAM_PID:
      param = static_cast<void *>(this->param->get_pid_information());
      break;
    case C2A_PARAM_CLI:
      param = static_cast<void *>(this->param->get_cli_parameter());
      break;
    case C2A_PARAM_ASS:
      param = static_cast<void *>(this->param->get_ass_setting());
      break;
    default:
      break;
    }
    return param;
  }
};

class IOutputHandler
{
public:
  int             active;
  DWORD           index;

protected:
  format_type     format;
  TCHAR          *name;
  FILE           *fp;
  size_t          string_length;
  IAppHandler    *app;
  time_t          timeLastFlush;

protected:
  void initialize(format_type format = FORMAT_INVALID, IAppHandler *app = NULL)
  {
    this->active = 0;
    this->format = format;
    this->name = NULL;
    this->fp = NULL;
    this->index = 0;
    this->string_length = MAX_PATH;
    this->app = app;
    this->timeLastFlush = time(NULL);
  }
  void release(void)
  {
    SAFE_DELETE_ARRAY(this->name);
  }
  void set_name(TCHAR *name, TCHAR *ext)
  {
    // Copy the specified strings to the output name.
    _tcscpy_s(this->name, this->string_length, name);
    _tcscat_s(this->name, this->string_length, ext);
  }
  int open(TCHAR *file_type)
  {
    if (this->fp)
      return -2;
    if (_tcsicmp(this->name, _T("")) == 0)
      return 1;

    this->fp = _tfsopen(this->name, _T("wb"), _SH_DENYWR);
    if (this->fp == NULL) {
      _tMyPrintf(_T("Open %s File: %s failed\r\n"), file_type, this->name);
      return -1;
    }

    this->index = 1;
    this->active = 1;
    return 0;
  }
  void close(int removed)
  {
    if (this->fp) {
      fclose(this->fp);
      if (removed) {

        /*他のプロセスから読み込まれている間は削除できない。数回繰り返す。*/
        for (size_t i = 0; i < 3; i++)
        {
          Sleep(100);
          if (remove(this->name) == 0) break;
        }
      }
      this->fp = NULL;
    }
  }
  void write_bom(void)
  {
    fwrite(utf8_bom, 3, 1, this->fp);
  }

public:
  virtual void SetName(void) = 0;
  virtual int  Open(void) = 0;
  virtual void WriteHeader(void) = 0;
  virtual void Close(void) = 0;

  int Allocate(size_t string_length = 0)
  {
    if (this->string_length < string_length)
      this->string_length = string_length;
    this->name = new TCHAR[this->string_length];
    if (!(this->name))
      return -1;
    memset(this->name, 0, sizeof(TCHAR) * this->string_length);
    return 0;
  }
};

class ICaptionHandler : public IOutputHandler
{
protected:
  int count_UTF8(const unsigned char *string);

public:
  virtual int  Setup(void) = 0;
  virtual void Dump(CAPTION_LIST& capList, DWORD endTime) = 0;
};

class ILogHandler : public IOutputHandler
{
public:
  void print(LPCTSTR msg, ...)
  {
    va_list ptr;
    va_start(ptr, msg);
    vfprintf(this->fp, msg, ptr);
    va_end(ptr);
  }
};

class CLogHandler : public ILogHandler
{
public:
  CLogHandler(IAppHandler *app)
  {
    this->initialize(FORMAT_LOG, app);
  }
  ~CLogHandler(void)
  {
    this->release();
  }

  void SetName(void);
  int  Open(void);
  void Close(void);
  void WriteHeader(void);
};

class CAssHandler : public ICaptionHandler
{
public:
  BOOL            norubi;

private:
  ass_setting_t  *as;

public:
  CAssHandler(IAppHandler *app)
    : norubi(FALSE), as(NULL)
  {
    this->initialize(FORMAT_ASS, app);
  }
  ~CAssHandler(void)
  {
    this->release();
  }

  void SetName(void);
  int  Open(void);
  void WriteHeader(void);
  void Close(void);
  int  Setup(void);
  void Dump(CAPTION_LIST& capList, DWORD endTime);
};

class CSrtHandler : public ICaptionHandler
{
public:
  BOOL            ornament;

public:
  CSrtHandler(IAppHandler *app)
    : ornament(FALSE)
  {
    this->initialize(FORMAT_SRT, app);
  }
  ~CSrtHandler(void)
  {
    this->release();
  }

  void SetName(void);
  int  Open(void);
  void WriteHeader(void);
  void Close(void);
  int  Setup(void);
  void Dump(CAPTION_LIST& capList, DWORD endTime);
};

class CAppHandler : public IAppHandler
{
private:
  static const int    output_type_max = 3 + 1;
  static const int    caption_type_max = 2 + 1;

public:
  // File handlers
  FILE               *fpInputTs;
  // Caption handlers
  ILogHandler        *log_handle;
  IOutputHandler     *output_handle[output_type_max];
  ICaptionHandler    *caption_handle[caption_type_max];

public:
  CAppHandler(void)
    : fpInputTs(NULL), log_handle(NULL)
  {
    this->initialize();
    memset(this->output_handle, 0, sizeof(IOutputHandler  *) * (this->output_type_max));
    memset(this->caption_handle, 0, sizeof(ICaptionHandler *) * (this->caption_type_max));
  }
  ~CAppHandler(void)
  {
    this->Free();
  }

  int Allocate(size_t string_length);
  void Free(void);
};

int CAppHandler::Allocate(size_t string_length)
{
  CCaption2AssParameter *param = NULL;
  CLogHandler           *log = NULL;
  CAssHandler           *ass = NULL;
  CSrtHandler           *srt = NULL;

  // Create parameter handler.
  param = new CCaption2AssParameter();
  if (!param) {
    _tMyPrintf(_T("Failed to allocate the paramter handler.\r\n"));
    goto ERR_EXIT;
  }
  if (param->Allocate(string_length)) {
    _tMyPrintf(_T("Failed to allocate the buffers for output.\r\n"));
    goto ERR_EXIT;
  }

  // Create caption handlers.
  cli_parameter_t *cp = param->get_cli_parameter();
  log = new CLogHandler(this);
  ass = new CAssHandler(this);
  srt = new CSrtHandler(this);
  if (!(log) || !(ass) || !(srt)) {
    _tMyPrintf(_T("Failed to allocate the caption handlers.\r\n"));
    goto ERR_EXIT;
  }
  if (log->Allocate(string_length) || ass->Allocate(string_length) || srt->Allocate(string_length)) {
    _tMyPrintf(_T("Failed to allocate the buffers for caption names.\r\n"));
    goto ERR_EXIT;
  }

  IOutputHandler   *output_handle[] = { ass, srt, log, NULL };
  ICaptionHandler *caption_handle[] = { ass, srt, NULL };

  // Setup.
  this->string_length = string_length;
  this->param = param;
  this->log_handle = log;
  memcpy(this->output_handle, output_handle, sizeof(IOutputHandler  *) * (this->output_type_max));
  memcpy(this->caption_handle, caption_handle, sizeof(ICaptionHandler *) * (this->caption_type_max));
  return 0;

ERR_EXIT:
  SAFE_DELETE(log);
  SAFE_DELETE(ass);
  SAFE_DELETE(srt);
  SAFE_DELETE(param);
  return -1;
}

void CAppHandler::Free(void)
{
  SAFE_DELETE(this->param);
  for (int i = 0; this->output_handle[i]; i++)
    SAFE_DELETE(this->output_handle[i]);
  for (int i = 0; this->caption_handle[i]; i++)
    caption_handle[i] = NULL;
  this->log_handle = NULL;
}

int ICaptionHandler::count_UTF8(const unsigned char *string)
{
  int len = 0;

  while (*string) {
    if (string[0] == 0x00)
      break;

    if (string[0] < 0x1f || string[0] == 0x7f) {
      // 制御コード
    }
    else {
      if (string[0] <= 0x7f)
        ++len; // 1バイト文字
      else if (string[0] <= 0xbf)
        ; // 文字の続き
      else if (string[0] <= 0xdf) {
        ++len; // 2バイト文字
        ++len; // 2バイト文字
        if ((string[0] == 0xc2) && (string[1] == 0xa5))
          --len; // 2バイト文字
      }
      else if (string[0] <= 0xef) {
        ++len; // 3バイト文字
        ++len; // 3バイト文字
        if ((string[0] == 0xe2) && (string[1] == 0x80) && (string[2] == 0xbe))
          --len; // 2バイト文字
        if (string[0] == 0xef) {
          if (string[1] == 0xbd)
            if ((string[2] >= 0xa1) && (string[2] == 0xbf))
              --len; // 2バイト文字
          if (string[1] == 0xbe)
            if ((string[2] >= 0x80) && (string[2] == 0x9f))
              --len; // 2バイト文字
        }
      }
      else if (string[0] <= 0xf7) {
        ++len; // 4バイト文字
        ++len; // 4バイト文字
      }
      else if (string[0] <= 0xfb) {
        ++len; // 5バイト文字
        ++len; // 5バイト文字
      }
      else if (string[0] <= 0xfd) {
        ++len; // 6バイト文字
        ++len; // 6バイト文字
      }
      else
        ; // 使われていない範囲
    }
    ++string;
  }

  return len;
}

void CLogHandler::SetName(void)
{
  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));
  if (!(cp->LogMode))
    return;
  // Set the log filename.
  TCHAR *name = cp->LogFileName;
  TCHAR *ext = NULL;
  if (_tcsicmp(name, _T("")) == 0) {
    name = cp->TargetFileName;
    ext = _T("_Caption.log");
  }
  this->set_name(name, ext);
}

void CAssHandler::SetName(void)
{
  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));
  if ((cp->format != FORMAT_ASS) && (cp->format != FORMAT_DUAL))
    return;
  // Set the ass filename.
  this->set_name(cp->TargetFileName, _T(".ass"));
}

void CSrtHandler::SetName(void)
{
  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));
  if ((cp->format != FORMAT_SRT) && (cp->format != FORMAT_TAW) && (cp->format != FORMAT_DUAL))
    return;
  // Set the srt filename.
  this->set_name(cp->TargetFileName, _T(".srt"));
}

int CLogHandler::Open(void)
{
  if (this->open(_T("Log")) < 0)
    return -1;
  return 0;
}

int CAssHandler::Open(void)
{
  int ret = this->open(_T("Target"));
  if (ret > 0)
    return 0;
  else if (ret < 0)
    return -1;

  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));

  // Initialize.
  this->norubi = cp->norubi;
  return 0;
}

int CSrtHandler::Open(void)
{
  int ret = this->open(_T("Target"));
  if (ret > 0)
    return 0;
  else if (ret < 0)
    return -1;

  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));

  // Initialize.
  this->ornament = (cp->format == FORMAT_TAW) ? FALSE : cp->srtornament;
  return 0;
}

void CLogHandler::WriteHeader(void)
{
  if (!(this->fp))
    return;
  if (this->app->bUnicode)
    this->write_bom();
}

void CAssHandler::WriteHeader(void)
{
  if (!(this->fp))
    return;
  this->write_bom();
  assHeaderWrite(this->fp, this->as);
}

void CSrtHandler::WriteHeader(void)
{
  if (!(this->fp) || this->format == FORMAT_TAW)
    return;
  this->write_bom();
}

void CLogHandler::Close(void)
{
  int removed = this->index == 0;
  this->close(removed);
}

void CAssHandler::Close(void)
{
  int removed = this->index == 1;
  this->close(removed);
}

void CSrtHandler::Close(void)
{
  int removed = this->index == 1;
  this->close(removed);
}

int CAssHandler::Setup(void)
{
  if (!(this->active))
    return 1;

  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));
  ass_setting_t   *as = static_cast<ass_setting_t *>(this->app->GetParam(C2A_PARAM_ASS));

  // Read ini settings.
  if (IniFileRead(cp->ass_type, as))
    return -1;

  // Check the resolution conversion.
  if ((as->PlayResX * 3) == (as->PlayResY * 4)) {
    this->app->sidebar_size = (((as->PlayResY * 16) / 9) - as->PlayResX) / 2;
    as->PlayResX = (as->PlayResY * 16) / 9;
  }

  // Setup ass settings.
  this->as = as;
  return 0;
}

int CSrtHandler::Setup(void)
{
  if (!(this->active))
    return 1;

  cli_parameter_t *cp = static_cast<cli_parameter_t *>(this->app->GetParam(C2A_PARAM_CLI));

  // Setup format.
  this->format = (cp->format != FORMAT_TAW) ? FORMAT_SRT : FORMAT_TAW;
  return 0;
}

void CAssHandler::Dump(CAPTION_LIST& capList, DWORD endTime)
{
  FILE *fp = this->fp;

  CAPTION_LIST::iterator it = capList.begin();
  for (int i = 0; it != capList.end(); it++, i++) {
    (*it)->endTime = endTime;

    unsigned short sH, sM, sS, sMs, eH, eM, eS, eMs;
    HMS((*it)->startTime, sH, sM, sS, sMs);
    HMS((*it)->endTime, eH, eM, eS, eMs);
    sMs /= 10;
    eMs /= 10;

    STRINGS_LIST::iterator it2 = (*it)->outStrings.begin();

    if (((*it)->outCharSizeMode != STR_SMALL) && ((*it)->outHLC == HLC_box)) {
      int iHankaku;
      unsigned char usTmpUTF8[STRING_BUFFER_SIZE] = { 0 };
      memcpy_s(usTmpUTF8, STRING_BUFFER_SIZE, (*it2)->str.c_str(), (*it2)->str.size());
      iHankaku = this->count_UTF8(usTmpUTF8);
      int iBoxPosX = (*it)->outPosX + (iHankaku * (((*it)->outCharW + (*it)->outCharHInterval) / 4)) - ((*it)->outCharHInterval / 4);
      int iBoxPosY = (*it)->outPosY + ((*it)->outCharVInterval / 2);
      int iBoxScaleX = (iHankaku + 1) * 50;
      int iBoxScaleY = 100 * ((*it)->outCharH + (*it)->outCharVInterval) / (*it)->outCharH;
      fprintf(fp, "Dialogue: 0,%01d:%02d:%02d.%02d,%01d:%02d:%02d.%02d,Box,,0000,0000,0000,,{\\pos(%d,%d)\\fscx%d\\fscy%d\\3c&H%06x&}",
        sH, sM, sS, sMs, eH, eM, eS, eMs, iBoxPosX, iBoxPosY, iBoxScaleX, iBoxScaleY, (*it2)->outCharColor);
      static const unsigned char utf8box[] = { 0xE2, 0x96, 0xA0 };
      fwrite(utf8box, 3, 1, fp);
      fprintf(fp, "\r\n");
    }
    if (((*it)->outCharSizeMode != STR_SMALL) && ((*it)->outHLC == HLC_draw)) {
      int iHankaku;
      unsigned char usTmpUTF8[STRING_BUFFER_SIZE] = { 0 };
      memcpy_s(usTmpUTF8, STRING_BUFFER_SIZE, (*it2)->str.c_str(), (*it2)->str.size());
      iHankaku = this->count_UTF8(usTmpUTF8);
      int iBoxPosX = (*it)->outPosX + (iHankaku * (((*it)->outCharW + (*it)->outCharHInterval) / 4));
      int iBoxPosY = (*it)->outPosY + ((*it)->outCharVInterval / 4);
      int iBoxScaleX = iHankaku * 55;
      int iBoxScaleY = 100;   //*((*it)->outCharH + (*it)->outCharVInterval) / (*it)->outCharH;
      fprintf(fp, "Dialogue: 0,%01d:%02d:%02d.%02d,%01d:%02d:%02d.%02d,Box,,0000,0000,0000,,{\\pos(%d,%d)\\3c&H%06x&\\p1}m 0 0 l %d 0 %d %d 0 %d{\\p0}\r\n",
        sH, sM, sS, sMs, eH, eM, eS, eMs, iBoxPosX, iBoxPosY, (*it2)->outCharColor, iBoxScaleX, iBoxScaleX, iBoxScaleY, iBoxScaleY);
    }
    if ((*it)->outCharSizeMode == STR_SMALL)
      fprintf(fp, "Dialogue: 0,%01d:%02d:%02d.%02d,%01d:%02d:%02d.%02d,Rubi,,0000,0000,0000,,{\\pos(%d,%d)",
      sH, sM, sS, sMs, eH, eM, eS, eMs, (*it)->outPosX, (*it)->outPosY);
    else
      fprintf(fp, "Dialogue: 0,%01d:%02d:%02d.%02d,%01d:%02d:%02d.%02d,Default,,0000,0000,0000,,{\\pos(%d,%d)",
      sH, sM, sS, sMs, eH, eM, eS, eMs, (*it)->outPosX, (*it)->outPosY);

    if ((*it2)->outCharColor.ucR != 0xff || (*it2)->outCharColor.ucG != 0xff || (*it2)->outCharColor.ucB != 0xff)
      fprintf(fp, "\\c&H%06x&", (*it2)->outCharColor);
    if ((*it2)->outUnderLine)
      fprintf(fp, "\\u1");
    if ((*it2)->outBold)
      fprintf(fp, "\\b1");
    if ((*it2)->outItalic)
      fprintf(fp, "\\i1");
    fprintf(fp, "}");

    if (((*it)->outCharSizeMode == STR_SMALL) && (this->norubi)) {
      fprintf(fp, "\\N");
    }
    else {
      if (((*it)->outCharSizeMode != STR_SMALL) && ((*it)->outHLC == HLC_kigou))
        fprintf(fp, "[");

      // Output strings.
      while (1) {
        fwrite((*it2)->str.c_str(), (*it2)->str.size(), 1, fp);

        PLINE_STR prev = *it2;
        ++it2;
        if (it2 == (*it)->outStrings.end())
          break;

        if (prev->outCharColor.ucR != (*it2)->outCharColor.ucR
          || prev->outCharColor.ucG != (*it2)->outCharColor.ucG
          || prev->outCharColor.ucB != (*it2)->outCharColor.ucB
          /* || prev->outCharColor.ucAlpha != (*it2)->outCharColor.ucAlpha */
          || prev->outUnderLine != (*it2)->outUnderLine
          || prev->outBold != (*it2)->outBold
          || prev->outItalic != (*it2)->outItalic) {
          fprintf(fp, "{");
          if (prev->outCharColor.ucR != (*it2)->outCharColor.ucR
            || prev->outCharColor.ucG != (*it2)->outCharColor.ucG
            || prev->outCharColor.ucB != (*it2)->outCharColor.ucB
            /* || prev->outCharColor.ucAlpha != (*it2)->outCharColor.ucAlpha */)
            fprintf(fp, "\\c&H%06x&", (*it2)->outCharColor);
          if (prev->outUnderLine != (*it2)->outUnderLine)
            fprintf(fp, "\\u%d", (*it2)->outUnderLine ? 1 : 0);
          if (prev->outBold != (*it2)->outBold)
            fprintf(fp, "\\b%d", (*it2)->outBold ? 1 : 0);
          if (prev->outItalic != (*it2)->outItalic)
            fprintf(fp, "\\i%d", (*it2)->outItalic ? 1 : 0);
          fprintf(fp, "}");
        }
      }

      if (((*it)->outCharSizeMode != STR_SMALL) && ((*it)->outHLC == HLC_kigou))
        fprintf(fp, "]");
      fprintf(fp, "\\N");
    }
    fprintf(fp, "\r\n");
  }

  if (capList.size() > 0)
    ++(this->index);

  // flush at least every 6 sec
  if (6 < time(NULL) - timeLastFlush)
  {
    fflush(fp);
    timeLastFlush = time(NULL);
  }
}

void CSrtHandler::Dump(CAPTION_LIST& capList, DWORD endTime)
{
#define ORNAMENT_START(s)                               \
do {                                                    \
    bItalic    =  (s)->outItalic;                       \
    bBold      =  (s)->outBold;                         \
    bUnderLine =  (s)->outUnderLine;                    \
    bCharColor = ((s)->outCharColor.ucR != 0xff         \
               || (s)->outCharColor.ucG != 0xff         \
               || (s)->outCharColor.ucB != 0xff)        \
               ? TRUE : FALSE;                          \
    if (bItalic)                                        \
        fprintf(fp, "<i>");                             \
    if (bBold)                                          \
        fprintf(fp, "<b>");                             \
    if (bUnderLine)                                     \
        fprintf(fp, "<u>");                             \
    if (bCharColor)                                     \
        fprintf(fp, "<font color=\"#%02x%02x%02x\">",   \
                    (s)->outCharColor.ucR,              \
                    (s)->outCharColor.ucG,              \
                    (s)->outCharColor.ucB);             \
} while (0)
#define ORNAMENT_END()          \
do {                            \
    if (bItalic)                \
        fprintf(fp, "</i>");    \
    if (bBold)                  \
        fprintf(fp, "</b>");    \
    if (bUnderLine)             \
        fprintf(fp, "</u>");    \
    if (bCharColor)             \
        fprintf(fp, "</font>"); \
} while (0)

  FILE *fp = this->fp;

  BOOL bNoSRT = TRUE;
  CAPTION_LIST::iterator it = capList.begin();
  for (int i = 0; it != capList.end(); it++, i++) {
    if (i == 0) {
      (*it)->endTime = endTime;

      unsigned short sH, sM, sS, sMs, eH, eM, eS, eMs;
      HMS((*it)->startTime, sH, sM, sS, sMs);
      HMS((*it)->endTime, eH, eM, eS, eMs);

      fprintf(fp, "%d\r\n%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\r\n", this->index, sH, sM, sS, sMs, eH, eM, eS, eMs);
    }

    // ふりがな Skip
    if ((*it)->outCharSizeMode == STR_SMALL)
      continue;
    bNoSRT = FALSE;

    STRINGS_LIST::iterator it2 = (*it)->outStrings.begin();
    BOOL bItalic = FALSE, bBold = FALSE, bUnderLine = FALSE, bCharColor = FALSE;

    if (this->ornament) {
      ORNAMENT_START(*it2);
    }
    if ((*it)->outHLC != 0)
      fprintf(fp, "[");
    // Output strings.
    while (1) {
      fwrite((*it2)->str.c_str(), (*it2)->str.size(), 1, fp);

      ++it2;
      if (it2 == (*it)->outStrings.end())
        break;

      if (this->ornament) {
        ORNAMENT_END();
        // Next ornament.
        ORNAMENT_START(*it2);
      }
    }

    if ((*it)->outHLC != 0)
      fprintf(fp, "]");
    if (this->ornament) {
      ORNAMENT_END();
    }
    fprintf(fp, "\r\n");
  }

  if (capList.size() > 0) {
    if (bNoSRT)
      fprintf(fp, "\r\n");
    fprintf(fp, "\r\n");
    ++(this->index);
  }

  // flush at least every 6 sec
  if (6 < time(NULL) - timeLastFlush)
  {
    fflush(fp);
    timeLastFlush = time(NULL);
  }
#undef ORNAMENT_START
#undef ORNAMENT_END
}

static void output_header(CAppHandler& app)
{
  IOutputHandler **handle = app.output_handle;

  // Output the header to the output files.
  for (int i = 0; handle[i]; i++)
    handle[i]->WriteHeader();
}

static int open_output_files(CAppHandler& app)
{
  IOutputHandler **handle = app.output_handle;

  // Open the output files.
  for (int i = 0; handle[i]; i++)
    if (handle[i]->Open())
      goto ERR_EXIT;

  return 0;

ERR_EXIT:
  return -1;
}

static int setup_output_settings(CAppHandler& app)
{
  ICaptionHandler **handle = app.caption_handle;

  for (int i = 0; handle[i]; i++)
    if (handle[i]->Setup() < 0)
      goto ERR_EXIT;

  return 0;

ERR_EXIT:
  return -1;
}

static void setup_output_filename(CAppHandler& app)
{
  cli_parameter_t *cp = static_cast<cli_parameter_t *>(app.GetParam(C2A_PARAM_CLI));
  size_t string_length = app.string_length;

  IOutputHandler **handle = app.output_handle;

  // Check the target name.
  if (_tcsicmp(cp->TargetFileName, _T("")) == 0)
    _tcscpy_s(cp->TargetFileName, string_length, cp->FileName);
  TCHAR *pExt = PathFindExtension(cp->TargetFileName);
  if (pExt && _tcsicmp(pExt, _T(".ts")) == 0)
    _tcscpy_s(pExt, 4, _T("\0"));

  // Set the output filenames.
  for (int i = 0; handle[i]; i++)
    handle[i]->SetName();

  return;
}

static void close_files(CAppHandler& app)
{
  IOutputHandler **handle = app.output_handle;

  // Close input file.
  if (app.fpInputTs) {
    fclose(app.fpInputTs);
    app.fpInputTs = NULL;
  }

  // Close output file.
  for (int i = 0; handle[i]; i++)
    handle[i]->Close();
}

static int initialize_caption_dll(CAppHandler& app, CCaptionDllUtil& capUtil)
{
  cli_parameter_t *cp = static_cast<cli_parameter_t *>(app.GetParam(C2A_PARAM_CLI));

  if (!capUtil.CheckUNICODE() || (cp->format == FORMAT_TAW)) {
    if (capUtil.Initialize() != NO_ERR)
      return -1;
    app.bUnicode = FALSE;
  }
  else {
    if (capUtil.InitializeUNICODE() != NO_ERR)
      return -1;
    app.bUnicode = TRUE;
  }
  return 0;
}

static int prepare_app_handler(CAppHandler& app, int argc, _TCHAR **argv)
{
  size_t string_length = MAX_PATH;

  // Check max of string length.
  for (int i = 0; i < argc; i++) {
    size_t length = _tcslen(argv[i]) + 1 + 20;  // +20: It's a margin for append the suffix.
    if (length > string_length)
      string_length = length;
  }

  // Prepare the caption handlers.
  return app.Allocate(string_length);
}

static void free_app_handler(CAppHandler& app)
{
  app.Free();
}

static void clear_caption_list(CAPTION_LIST& capList)
{
  if (capList.empty())
    return;
  for (CAPTION_LIST::iterator it = capList.begin(); it != capList.end(); ++it) {
    if (!((*it)->outStrings.empty()))
      for (STRINGS_LIST::iterator it2 = (*it)->outStrings.begin(); it2 != (*it)->outStrings.end(); ++it2)
        delete *it2;
    delete *it;
  }
  capList.clear();
}

static int output_caption(CAppHandler& app, CCaptionDllUtil& capUtil, CAPTION_LIST& capList, long long PTS)
{
  int   workCharSizeMode = 0;
  int   workCharW = 0;
  int   workCharH = 0;
  int   workCharHInterval = 0;
  int   workCharVInterval = 0;
  int   workPosX = 0;
  int   workPosY = 0;
  BYTE  workHLC = HLC_kigou;    //must ignore low 4bits
  WORD  wLastSWFMode = 999;
  int   offsetPosX = 0;
  int   offsetPosY = 0;
  float ratioX = 2;
  float ratioY = 2;

  // Prepare the handlers.
  cli_parameter_t *cp = static_cast<cli_parameter_t *>(app.GetParam(C2A_PARAM_CLI));
  ass_setting_t   *as = static_cast<ass_setting_t *>(app.GetParam(C2A_PARAM_ASS));
  ILogHandler     *log = app.log_handle;

  ICaptionHandler **handle = app.caption_handle;

  // Output
  std::vector<CAPTION_DATA> Captions;
  int ret = capUtil.GetCaptionData(0, &Captions);

  std::vector<CAPTION_DATA>::iterator it = Captions.begin();
  for (; it != Captions.end(); it++) {
    if (it->bClear) {
      // 字幕のスキップをチェック
      if ((PTS + it->dwWaitTime) <= app.startPCR) {
        _tMyPrintf(_T("%d Caption skip\r\n"), capList.size());
        if (log->active)
          log->print("%d Caption skip\r\n", capList.size());
        clear_caption_list(capList);
        continue;
      }

      //capList.size()を確認するようにした。
      //　空のcapListに反応して、cp->detectLengthの処理中断ができず
      //　空の字幕ファイルだけが残る。字幕が無いものとみなす。
      if (0 < capList.size())  app.bCreateOutput = TRUE;


      DWORD endTime = (DWORD)((PTS + it->dwWaitTime) - app.startPCR);
      for (int i = 0; handle[i]; i++)
        if (handle[i]->active)
          handle[i]->Dump(capList, endTime);
      clear_caption_list(capList);

      continue;
    }
    else {
      std::vector<CAPTION_CHAR_DATA>::iterator it2 = it->CharList.begin();

      if (log->active) {
        log->print("SWFMode    : %4d\r\n", it->wSWFMode);
        log->print("Client X:Y : %4d\t%4d\r\n", it->wClientX, it->wClientY);
        log->print("Client W:H : %4d\t%4d\r\n", it->wClientW, it->wClientH);
        log->print("Pos    X:Y : %4d\t%4d\r\n", it->wPosX, it->wPosY);
      }

      if (it->wSWFMode != wLastSWFMode) {
        wLastSWFMode = it->wSWFMode;
        static const struct {
          int x;
          int y;
        } resolution[4] = {
          { 1920, 1080 }, { 720, 480 }, { 1280, 720 }, { 960, 540 }
        };
        int index = (wLastSWFMode == 5) ? 0
          : (wLastSWFMode == 9) ? 1
          : (wLastSWFMode == 11) ? 2
          : 3;
        ratioX = (float)(as->PlayResX) / (float)(resolution[index].x);
        ratioY = (float)(as->PlayResY) / (float)(resolution[index].y);
      }
      if (app.bUnicode) {
        if ((it->wPosX < 2000) || (it->wPosY < 2000)) {
          offsetPosX = it->wClientX;
          offsetPosY = it->wClientY;
        }
        else {
          offsetPosX = 0;
          offsetPosY = 0;
          it->wPosX -= 2000;
          it->wPosY -= 2000;
        }
      }

      if (it->CharList.size() > 0) {
        workCharSizeMode = it2->emCharSizeMode;
        workCharW = it2->wCharW;
        workCharH = it2->wCharH;
        workCharHInterval = it2->wCharHInterval;
        workCharVInterval = it2->wCharVInterval;
        // Calculate offsetPos[X/Y].
        if (!(app.bUnicode)) {
          int amariPosX = 0;
          int amariPosY = 0;
          if (wLastSWFMode == 9) {
            amariPosX = it->wPosX % 18;
            amariPosY = it->wPosY % 15;
          }
          else {
            amariPosX = it->wPosX % ((workCharW + workCharHInterval) / 2);
            amariPosY = it->wPosY % ((workCharH + workCharVInterval) / 2);
          }
          if ((amariPosX == 0) || (amariPosY == 0)) {
            offsetPosX = it->wClientX;
            offsetPosY = it->wClientY + 10;
          }
          else {
            offsetPosX = 0;
            offsetPosY = 0;
          }
        }
        // Calculate workPos[X/Y].
        int   y_swf_offset = 0;
        float x_ratio = ratioX;
        float y_ratio = ratioY;
        switch (wLastSWFMode) {
        case 0:
          y_swf_offset = as->SWF0offset;
          break;
        case 5:
          y_swf_offset = as->SWF5offset /* - 0 */;
          break;
        case 7:
          y_swf_offset = as->SWF7offset /* + 0 */;
          break;
        case 9:
          y_swf_offset = as->SWF9offset + ((app.bUnicode) ? 0 : -50);
          break;
        case 11:
          y_swf_offset = as->SWF11offset /* - 0 */;
          break;
        default:
          x_ratio = y_ratio = 1.0;
          break;
        }
        workPosX = (int)((float)(it->wPosX + offsetPosX) * x_ratio);
        workPosY = (int)((float)(it->wPosY + offsetPosY + y_swf_offset) * y_ratio);
        // Correction for workPosX.
        workPosX = (workPosX > app.sidebar_size) ? workPosX - app.sidebar_size : 0;

        if (!(app.bUnicode) && (it2->emCharSizeMode == STR_SMALL))
          workPosY += (int)(10 * ratioY);
      }

      PCAPTION_LINE pCapLine = new CAPTION_LINE();
      if (!pCapLine)
        goto ERR_EXIT;

      for (; it2 != it->CharList.end(); it2++) {
        PLINE_STR pLineStr = new LINE_STR();
        if (!pLineStr) {
          delete pCapLine;
          goto ERR_EXIT;
        }

        unsigned char workucR = it2->stCharColor.ucR;
        unsigned char workucG = it2->stCharColor.ucG;
        unsigned char workucB = it2->stCharColor.ucB;
        BOOL workUnderLine = it2->bUnderLine;
        BOOL workShadow = it2->bShadow;
        BOOL workBold = it2->bBold;
        BOOL workItalic = it2->bItalic;
        BYTE workFlushMode = it2->bFlushMode;
        workHLC = (it2->bHLC != 0) ? cp->HLCmode : it2->bHLC;

        if (!(app.bUnicode) && (it2->emCharSizeMode == STR_MEDIUM))
          // 全角 -> 半角
          it2->strDecode = GetHalfChar(it2->strDecode);

        if (log->active) {
          if (it2->bUnderLine)
            log->print("UnderLine : on\r\n");
          if (it2->bBold)
            log->print("Bold : on\r\n");
          if (it2->bItalic)
            log->print("Italic : on\r\n");
          if (it2->bHLC != 0)
            log->print("HLC : on\r\n");
          log->print("Color : %#.X   ", it2->stCharColor);
          log->print("Char M,W,H,HI,VI : %4d, %4d, %4d, %4d, %4d   ",
            it2->emCharSizeMode, it2->wCharW, it2->wCharH, it2->wCharHInterval, it2->wCharVInterval);
          log->print("%s\r\n", it2->strDecode.c_str());
        }

        CHAR  str_utf8[STRING_BUFFER_SIZE] = { 0 };
        WCHAR str_wchar[STRING_BUFFER_SIZE] = { 0 };

        if ((cp->format == FORMAT_TAW) || (app.bUnicode))
          strcpy_s(str_utf8, STRING_BUFFER_SIZE, it2->strDecode.c_str());
        else {
          // CP 932 to UTF-8
          MultiByteToWideChar(932, 0, it2->strDecode.c_str(), -1, str_wchar, STRING_BUFFER_SIZE);
          WideCharToMultiByte(CP_UTF8, 0, str_wchar, -1, str_utf8, STRING_BUFFER_SIZE, NULL, NULL);
        }

        // Push back the caption strings.
        pLineStr->outCharColor.ucAlpha = 0x00;
        pLineStr->outCharColor.ucR = workucR;
        pLineStr->outCharColor.ucG = workucG;
        pLineStr->outCharColor.ucB = workucB;
        pLineStr->outUnderLine = workUnderLine;
        pLineStr->outShadow = workShadow;
        pLineStr->outBold = workBold;
        pLineStr->outItalic = workItalic;
        pLineStr->outFlushMode = workFlushMode;
        pLineStr->str = str_utf8;

        pCapLine->outStrings.push_back(pLineStr);
      }

      if (pCapLine->outStrings.empty()) {
        delete pCapLine;
        continue;
      }

      // Push back the caption lines.
      pCapLine->index = 0;     //useless
      pCapLine->startTime = (PTS > app.startPCR) ? (DWORD)(PTS - app.startPCR) : 0;
      pCapLine->endTime = 0;
      pCapLine->outCharSizeMode = workCharSizeMode;
      pCapLine->outCharW = (WORD)(workCharW * ratioX);
      pCapLine->outCharH = (WORD)(workCharH * ratioY);
      pCapLine->outCharHInterval = (WORD)(workCharHInterval * ratioX);
      pCapLine->outCharVInterval = (WORD)(workCharVInterval * ratioY);
      pCapLine->outHLC = workHLC;
      pCapLine->outPosX = workPosX;
      pCapLine->outPosY = workPosY;

      capList.push_back(pCapLine);
    }
  }
  return 0;

ERR_EXIT:
  return -1;
}

static int main_loop(CAppHandler& app, CCaptionDllUtil& capUtil, CAPTION_LIST& capList)
{
  int             result = C2A_SUCCESS;
  ILogHandler       *log = app.log_handle;
  pid_information_t  *pi = static_cast<pid_information_t *>(app.GetParam(C2A_PARAM_PID));
  cli_parameter_t    *cp = static_cast<cli_parameter_t *>(app.GetParam(C2A_PARAM_CLI));

  BOOL bPrintPMT = TRUE;
  BYTE pbPacket[188 * 2 + 4] = { 0 };
  DWORD packetCount = 0;

  const int TSPacketSize = 188;
  bool afterResync = false;                                          //Resyncの直後？  Resync時に同期バイト'G'は取得済み
  auto tickBeginTime = system_clock::now();                          //速度制限    計測開始時間
  double tickReadSize = 0;                                           //            200ms間の読込み量
  double limit_Bsec = cp->ReadSpeedLimit_MiBsec * 1024 * 1024;       //            最大読込み速度

  //
  // Main loop
  //
  while (true){

    size_t read;
    if (!afterResync)
    {
      //通常
      read = fread(pbPacket, TSPacketSize, 1, app.fpInputTs);
    }
    else
    {
      //Resync直後
      //同期バイト'G'は既に確認済み。残りの187byteを読み込む。
      //ストリーム位置を戻せないのでResync直後は処理を分ける。
      pbPacket[0] = 'G';
      read = fread(&pbPacket[1], TSPacketSize - 1, 1, app.fpInputTs);
      afterResync = false;
    }
    if (read == 0) break;           //EOF or close pipe

    //速度制限
    if (cp->Mode_PipeInput == false && 0 < limit_Bsec)
    {
      tickReadSize += read * TSPacketSize;                                   //200mx間の読込み量
      auto tickElapse = system_clock::now() - tickBeginTime;                 //経過時間
      auto elapse_ms = duration_cast<chrono::milliseconds>(tickElapse).count();

      //200msごとにカウンタリセット
      if (200 <= elapse_ms)
      {
        tickBeginTime = system_clock::now();
        tickReadSize = 0;
      }

      //読込量が制限をこえたらsleep_for
      if (limit_Bsec * (200.0 / 1000.0) < tickReadSize)
        this_thread::sleep_for(chrono::milliseconds(200 - elapse_ms));
    }


    packetCount++;
    if (cp->detectLength > 0) {
      if (packetCount > cp->detectLength && !(app.bCreateOutput)) {
        _tMyPrintf(_T("Programme has deteced %dw packets, but can't find caption. Now it exits.\r\n"), packetCount / 10000);
        break;
      }
    }
    if (log->active) {
      if (packetCount < 100000) {
        if ((packetCount % 10000) == 0)
          log->print("Process  %dw packets.\r\n", packetCount / 10000);
      }
      else if (packetCount < 1000000) {
        if ((packetCount % 100000) == 0)
          log->print("Process  %dw packets.\r\n", packetCount / 10000);
      }
      else if (packetCount < 10000000) {
        if ((packetCount % 1000000) == 0)
          log->print("Process  %dw packets.\r\n", packetCount / 10000);
      }
      else {
        if ((packetCount % 10000000) == 0)
          log->print("Process  %dw packets.\r\n", packetCount / 10000);
      }
    }

    Packet_Header packet;
    parse_Packet_Header(&packet, &pbPacket[0]);

    if (packet.Sync != 'G')
    {
      if (!resync2(pbPacket, app.fpInputTs, TSPacketSize))
      {
        _tMyPrintf(_T("\r\n"));
        _tMyPrintf(_T("Fail to resync.\r\n"));
        result = C2A_FAILURE;
        Sleep(2000);
        goto EXIT;
      }
      //再同期後のパケットはpbPacketに読込み済、パースして続行
      afterResync = true;
      parse_Packet_Header(&packet, &pbPacket[0]);
    }

    if (packet.TsErr)
      continue;

    // PAT
    if (packet.PID == 0 && (pi->PMTPid == 0 || bPrintPMT)) {
      parse_PAT(&pbPacket[0], &(pi->PMTPid));
      bPrintPMT = FALSE;

      continue; // next packet
    }

    // PMT
    if (pi->PMTPid != 0 && packet.PID == pi->PMTPid) {
      if (pbPacket[5] != 0x02 || (pbPacket[6] & 0xf0) != 0xb0)
        /*--------------------------------------------------
        * pbPacket[5]  (8)  table_id
        * pbPacket[6]  (1)  section_syntax_indicator
        *              (1)  '0'
        *              (2)  reserved '11'
        *------------------------------------------------*/
        continue;   // next packet

      parse_PMT(&pbPacket[0], &(pi->PCRPid), &(pi->CaptionPid));

      if (log->active)
        if (app.lastPTS == TIMESTAMP_INVALID_VALUE)
          log->print("PMT, PCR, Caption : %04x, %04x, %04x\r\n", pi->PMTPid, pi->PCRPid, pi->CaptionPid);

      continue; // next packet
    }

    // PCR
    if (pi->PCRPid != 0 && packet.PID == pi->PCRPid) {
      DWORD bADP = (((DWORD)pbPacket[3] & 0x30) >> 4);
      if (!(bADP & 0x2))
        continue; // next packet

      DWORD bAF = (DWORD)pbPacket[5];
      if (!(bAF & 0x10))
        continue; // next packet

      // Get PCR.
      /*     90kHz           27MHz
      *  +--------+-------+-------+
      *  | 33 bits| 6 bits| 9 bits|
      *  +--------+-------+-------+
      */
      long long PCR, PCR_base, PCR_ext;
      PCR_base = ((long long)pbPacket[6] << 25)
        | ((long long)pbPacket[7] << 17)
        | ((long long)pbPacket[8] << 9)
        | ((long long)pbPacket[9] << 1)
        | ((long long)pbPacket[10] >> 7);
      PCR_ext = ((long long)(pbPacket[10] & 0x01) << 8)
        | (long long)pbPacket[11];
      PCR = (PCR_base * 300 + PCR_ext) / 27000;

      if (log->active)
        if (app.lastPTS == TIMESTAMP_INVALID_VALUE)
          log->print("PCR, startPCR, lastPCR, basePCR : %11lld, %11lld, %11lld, %11lld\r\n",
          PCR, app.startPCR, app.lastPCR, app.basePCR);

      // Check startPCR.
      if (app.startPCR == TIMESTAMP_INVALID_VALUE) {
        app.startPCR = PCR;
        app.correctTS = cp->DelayTime;
      }
      else {
        long long checkTS = 0;
        // Check wrap-around.
        if (PCR < app.lastPCR) {
          if (log->active) {
            log->print("====== PCR less than lastPCR ======\r\n");
            log->print("PCR, startPCR, lastPCR, basePCR : %11lld, %11lld, %11lld, %11lld\r\n",
              PCR, app.startPCR, app.lastPCR, app.basePCR);
          }
          app.basePCR += WRAP_AROUND_VALUE / 90;
          checkTS = WRAP_AROUND_VALUE / 90;
        }
        // Check drop packet. (This is even if the CM cut.)
        checkTS += PCR;
        if (checkTS > app.lastPCR) {
          checkTS -= app.lastPCR;
          if (!(cp->keepInterval) && (checkTS > PCR_MAXIMUM_INTERVAL))
            app.correctTS -= checkTS - (PCR_MAXIMUM_INTERVAL >> 2);
        }
      }

      // Update lastPCR.
      app.lastPCR = PCR;

      continue; // next packet
    }

    // Caption
    if (pi->CaptionPid != 0 && packet.PID == pi->CaptionPid) {
      long long PTS = 0;

      if (packet.PayloadStartFlag) {
#if 0
        // FIXME: Check PTS flag in PES Header.
        // [example]
        //if (!(packet.pts_flag))
        //    continue;
#endif

        // Get Caption PTS.
        PTS = GetPTS(pbPacket);
        if (log->active)
          log->print("PTS, lastPTS, basePTS, startPCR : %11lld, %11lld, %11lld, %11lld    ",
          PTS, app.lastPTS, app.basePTS, app.startPCR);

        // Check skip.
        if (PTS == TIMESTAMP_INVALID_VALUE || app.startPCR == TIMESTAMP_INVALID_VALUE) {
          if (log->active)
            log->print("Skip 1st caption\r\n");
          continue;
        }

        // Check wrap-around.
        // [case]
        //   lastPCR:  Detection on the 1st packet.             [1st PCR  >>> w-around >>> 1st PTS]
        //   lastPTS:  Detection on the packet of 2nd or later. [prev PTS >>> w-around >>> now PTS]
        long long checkTS = (app.lastPTS == TIMESTAMP_INVALID_VALUE) ? app.lastPCR : app.lastPTS;
        if ((PTS < checkTS) && ((checkTS - PTS) >(WRAP_AROUND_CHECK_VALUE / 90)))
          app.basePTS += WRAP_AROUND_VALUE / 90;

        // Update lastPTS.
        app.lastPTS = PTS;
      }
      else {
        if (log->active)
          log->print("PTS, lastPTS, basePTS, startPCR : %11lld, %11lld, %11lld, %11lld    ",
          PTS, app.lastPTS, app.basePTS, app.startPCR);

        // Check skip.
        if (app.lastPTS == TIMESTAMP_INVALID_VALUE || app.startPCR == TIMESTAMP_INVALID_VALUE) {
          if (log->active)
            log->print("Skip 2nd caption\r\n");
          continue;
        }

        // Get Caption PTS from 1st caption.
        PTS = app.lastPTS;
      }

      // Correct PTS for output.
      PTS += app.basePTS + app.correctTS;

      unsigned short sH, sM, sS, sMs;
      HMS((PTS > app.startPCR) ? (PTS - app.startPCR) : 0, sH, sM, sS, sMs);

      if (packet.PayloadStartFlag)
        _tMyPrintf(_T("Caption Time: %01d:%02d:%02d.%03d\r\n"), sH, sM, sS, sMs);
      if (log->active)
        log->print("%s Caption Time: %01d:%02d:%02d.%03d\r\n",
        ((packet.PayloadStartFlag) ? "1st" : "2nd"), sH, sM, sS, sMs);

      // Parse caption.
      int ret = capUtil.AddTSPacket(pbPacket);
      if (ret == CHANGE_VERSION) {
        std::vector<LANG_TAG_INFO> tagInfoList;
        ret = capUtil.GetTagInfo(&tagInfoList);
      }
      else if (ret == NO_ERR_CAPTION)
        if (output_caption(app, capUtil, capList, PTS)) {
          result = C2A_ERR_MEMORY;
          goto EXIT;
        }
    }
  }

EXIT:
  return result;
}



///  NonCaptionTag作成
void CreateNonCapTag(CAppHandler &app, TCHAR *targetName)
{
  //字幕があったか？
  //indexで判断、indexは初期化時に1、字幕を検出するたびに+1される。
  bool hasCap = false;
  ICaptionHandler** caphandle = app.caption_handle;
  for (int i = 0; caphandle[i]; i++)
  {
    if (caphandle[i]->index <= 1)
      continue;
    else
      hasCap = true;
  }

  if (hasCap == false)
  {
    TCHAR tagPath[1024] = {};
    _tcscat_s(tagPath, 1024, targetName);
    _tcscat_s(tagPath, 1024, _T(".noncap"));
    FILE *fp = _tfsopen(tagPath, _T("wb"), _SH_DENYWR); //共有設定  読込みを許可
    fclose(fp);
  }
}



// 
//command line example
//     -i "E:\TS_Samp\cap8s.ts"  -format srt  -NonCapTag
//
//
int _tmain(int argc, _TCHAR *argv[])
{
  int             result = C2A_SUCCESS;
  CCaptionDllUtil capUtil;
  CAPTION_LIST    capList;
  CAppHandler     app;

#ifdef _DEBUG
  int debug = 0;
  if (argc < 2)
    debug = load_debug(&argc, &argv);
#endif

  // Prepare the handlers.
  if (prepare_app_handler(app, argc, argv)) {
    result = C2A_ERR_MEMORY;
    Sleep(2000);
    goto EXIT;
  }
  CCaption2AssParameter *param = static_cast<CCaption2AssParameter *>(app.GetParam(C2A_PARAM_ALL));

  // Parse arguments.
  if (ParseCmd(argc, argv, param)) {
    result = C2A_ERR_PARAM;
    Sleep(2000 * 3);
    goto EXIT;
  }
  pid_information_t *pi = static_cast<pid_information_t *>(app.GetParam(C2A_PARAM_PID));
  cli_parameter_t   *cp = static_cast<cli_parameter_t *>(app.GetParam(C2A_PARAM_CLI));
  ass_setting_t     *as = static_cast<ass_setting_t *>(app.GetParam(C2A_PARAM_ASS));

  // Initialize Caption Utility.
  if (initialize_caption_dll(app, capUtil)) {
    _tMyPrintf(_T("\r\n"));
    _tMyPrintf(_T("Load Caption.dll failed\r\n"));
    result = C2A_ERR_DLL;
    Sleep(2000);
    goto EXIT;
  }

  // Initialize the output filename.
  setup_output_filename(app);
  static const TCHAR *format_name[FORMAT_MAX] = {
    _T(""),
    _T("srt"),
    _T("ass"),
    _T("srt for TAW"),
    _T("ass & srt")
  };
  _tMyPrintf(_T("[Source] %s\r\n"), cp->FileName);
  _tMyPrintf(_T("[Target] %s\r\n"), cp->TargetFileName);
  _tMyPrintf(_T("[Format] %s\r\n"), format_name[cp->format]);

  //Open File
  if (cp->Mode_PipeInput)
  {
    //stdin
    app.fpInputTs = stdin;
    _setmode(_fileno(stdin), _O_BINARY);
    setvbuf(stdin, NULL, _IOFBF, 1024 * 64);
  }
  else
  {
    //file
    if (_tfopen_s(&(app.fpInputTs), cp->FileName, _T("rb"))
      || !(app.fpInputTs))
    {
      _tMyPrintf(_T("\r\n"));
      _tMyPrintf(_T("Open TS File: %s failed\r\n"), cp->FileName);
      result = C2A_ERR_PARAM;
      Sleep(2000);
      goto EXIT;
    }
  }

  // Open ASS/SRT/Log File.
  if (open_output_files(app)) {
    result = C2A_FAILURE;
    goto EXIT;
  }

  // Setup the output setting.
  if (setup_output_settings(app)) {
    result = C2A_FAILURE;
    goto EXIT;
  }

  // Output the header.
  output_header(app);

  // Main loop
  result = main_loop(app, capUtil, capList);

  // NonCaptionTag作成
  if (cp->NonCaptionTag)
    CreateNonCapTag(app, cp->TargetFileName);


EXIT:
  clear_caption_list(capList);

  close_files(app);

  free_app_handler(app);

#ifdef _DEBUG
  if (debug)
    unload_debug(argc, argv);
#endif

  return result;
}