/*
 * pam_harness — PAM モジュール単体テスト用ハーネス (root 不要)
 *
 * /etc/pam.d/<service> を読んで pam_authenticate を実行し、
 * スタックの結果と モジュール自身の判定 (/tmp/inmu_pam_dummy.log の最新行) を表示する。
 * sudo の設定を一切触らずにダミーモジュールの動作を確認するためのもの。
 *
 * 使い方:
 *   ./pam_harness <service名> [ユーザー名]
 *
 * 例 (install.sh が /etc/pam.d/inmutest を作成した後):
 *   ./pam_harness inmutest    # → exit 1 (pam_deny が失敗させた = 期待値)
 *   sudo touch /tmp/inmu_pam_demo_success
 *   ./pam_harness inmutest    # → exit 0 (モジュールの sufficient が短絡した = 期待値)
 *   sudo rm /tmp/inmu_pam_demo_success
 *   ./pam_harness inmutest    # → exit 1 に戻る
 *
 * inmutest のスタック (install.sh が作成):
 *   auth      sufficient  pam_inmu_dummy.so debug demo_success
 *   auth      required    pam_deny.so
 *   account   required    pam_permit.so
 *
 * 出力の読み方:
 *   - pam_authenticate: スタック全体の結果 (exit コードの基準)
 *   - モジュールの判定ログ: モジュール自身が返した値 (PAM_IGNORE / PAM_SUCCESS / ...)
 *     ※ auth行1個だけのスタックでモジュールが PAM_IGNORE を返すと OpenPAM の
 *       集計は PAM_SUCCESS になるため、スタック結果だけでは判定できない。
 *       そのため pam_deny.so を壁に置き、モジュールの判定はログで確認する。
 *
 * 会話関数は PAM_PROMPT_* に対して環境変数 INMU_HARNESS_PASSWORD の値を返す。
 * 終了コード: 0 = pam_authenticate 成功 / 1 = 失敗 / 2 = pam_start 失敗
 */

#include <security/pam_appl.h>

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_PATH "/tmp/inmu_pam_dummy.log"

static const char *prompt_answer(void) {
    const char *pw = getenv("INMU_HARNESS_PASSWORD");
    return pw ? pw : "";
}

static int conv_func(int num_msg, const struct pam_message **msg,
                     struct pam_response **resp, void *appdata_ptr) {
    (void)appdata_ptr;
    struct pam_response *r = calloc((size_t)num_msg, sizeof(*r));
    if (r == NULL) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            r[i].resp = strdup(prompt_answer());
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

/* モジュールが最後に記録した判定行 ("-> PAM_..." を含む最新行) を表示する。
 * この実行とログを対応させるため、pid が一致しない行は「古いログ」と明示する。 */
static void print_module_decision(void) {
    FILE *f = fopen(LOG_PATH, "r");
    if (f == NULL) {
        printf("モジュールの判定ログ: (見つからない — モジュールが呼ばれていない可能性)\n");
        return;
    }
    char last[1024] = "";
    char line[1024];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, "-> PAM_") != NULL) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = '\0';
            }
            snprintf(last, sizeof last, "%s", line);
        }
    }
    fclose(f);
    if (last[0] != '\0') {
        char pid_tag[32];
        snprintf(pid_tag, sizeof pid_tag, "[pid %d]", (int)getpid());
        printf("モジュールの判定ログ: %s\n", last);
        if (strstr(last, pid_tag) == NULL) {
            printf("  ※注意: このログはこの実行 (pid %d) のものではありません (古いログ)\n",
                   (int)getpid());
        }
    } else {
        printf("モジュールの判定ログ: (ログにあるが判定行なし)\n");
    }
}

int main(int argc, char **argv) {
    const char *service = (argc > 1) ? argv[1] : "inmutest";
    const char *user = (argc > 2) ? argv[2] : NULL;

    if (user == NULL) {
        struct passwd *pw = getpwuid(getuid());
        user = (pw != NULL) ? pw->pw_name : "nobody";
    }

    pam_handle_t *pamh = NULL;
    struct pam_conv pc = {conv_func, NULL};

    int r = pam_start(service, user, &pc, &pamh);
    if (r != PAM_SUCCESS) {
        fprintf(stderr, "pam_start(%s) failed: %s\n", service, pam_strerror(pamh, r));
        return 2;
    }

    fprintf(stderr, "service=%s user=%s pid=%d で pam_authenticate を呼びます...\n",
            service, user, (int)getpid());
    r = pam_authenticate(pamh, 0);
    printf("pam_authenticate = %d (%s)\n", r, pam_strerror(pamh, r));

    /* accountフェーズは参考情報 (inmutest では pam_permit なので成功する)。
     * 終了コードには含めない — テスト対象は auth だけのため。 */
    int acct = pam_acct_mgmt(pamh, 0);
    printf("pam_acct_mgmt    = %d (%s)  [参考]\n", acct, pam_strerror(pamh, acct));

    print_module_decision();

    if (r == PAM_SUCCESS) {
        printf("=> 結果: 認証成功 (exit 0)\n");
    } else {
        printf("=> 結果: 認証失敗 (exit 1) — モジュールが無視/失敗した場合は pam_deny.so が確定させた期待値です\n");
    }

    pam_end(pamh, r);
    return (r == PAM_SUCCESS) ? 0 : 1;
}
