/* ft_engine.h — native wrapper around the FocalTech WinBio matcher DLL.
 * Loads ftWbioEngineAdapter.dll in-process (no Wine) and exposes enroll/verify.
 * Single-device (global state); fine for one FT9201 reader.
 * (ft9201-libfprint 由来、macOS移植版でも同一API) */
#ifndef FT_ENGINE_H
#define FT_ENGINE_H
#include <stddef.h>
#include <stdint.h>

/* Load + initialize the engine. dll_path = path to ftWbioEngineAdapter.dll.
 * Returns 0 on success.
 * DLL はプロセス内 (デーモンでは root) で実行されるため、ファイル全体の SHA-256 が
 * 実装にピン留めした値と一致する時だけロードする。シンボリックリンク・通常ファイル
 * 以外・(root 実行時) root 所有でない/他者が書き込めるファイルは拒否する。
 * 失敗時は 0 以外を返し、他の ft_engine_* は何もしない (verify は常に 0)。 */
int ft_engine_open(const char *dll_path);
void ft_engine_close(void);

/* The image geometry the engine matches at (64x80). Feed any size; it is
 * resized internally. */
void ft_engine_geometry(int *w, int *h);

/* Feed one 8-bit grayscale frame (sw x sh). purpose: 4=enroll, 1=verify.
 * Returns 0 on success (features extracted), else the engine HRESULT. */
uint32_t ft_engine_accept(const uint8_t *img, int sw, int sh, uint8_t purpose);

/* Enrollment: begin, then per accepted frame call update, then commit. */
void ft_engine_enroll_begin(void);
/* Fold the last accepted frame into the enroll template.
 * Returns: 0 = complete, 1 = need more samples, 2 = frame rejected. */
int ft_engine_enroll_update(void);
/* Commit the enrollment. On success (*out,*outlen) is a malloc'd template blob
 * the caller must free. Returns 0 on success. */
int ft_engine_enroll_commit(uint8_t **out, size_t *outlen);

/* テンプレート (enroll_commit が返し、verify に渡す blob) の最大バイト数。
 * これを超えるテンプレートを verify は照合せず 0 を返す。テンプレートを読み込む側
 * (inmu-cored) も同じ上限で拒否すること。 */
#define FT_ENGINE_TEMPLATE_MAX (1u << 20)

/* Verify: load a previously committed template, then match it against the
 * most recently accepted frame. Returns 1 = match, 0 = no match.
 * 1 を返すのはエンジンが S_OK かつ Match=TRUE を返した時だけ。エラー・未ロード・
 * 不正な引数はすべて 0 (fail-closed)。 */
int ft_engine_verify(const uint8_t *tmpl, size_t tmpllen);

#endif
