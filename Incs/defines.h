#include <syslog.h>

#define CUSTOM_USER_DIRECTORY  "/dev/null"
#define CUSTOM_PLUGIN_PATH     ""
#define PLUGIN_SAVE_PREF       "/purple/nullclient/plugins/saved"
#define UI_ID                  "adapter"

typedef struct _IOClosure
{
	guint result;
	gpointer data;
	PurpleInputFunction function; 
} IOClosure;

static void destroyNotify(gpointer dataToFree);
static gboolean adapterInvokeIO(GIOChannel *source, GIOCondition condition, gpointer data);
static guint adapterIOAdd(gint fd, PurpleInputCondition condition, PurpleInputFunction function, gpointer data);
static void adapterUIInit(void);
static GHashTable* getClientInfo(void);
static void incoming_message_cb(PurpleConversation *conv, const char *who, const char *alias, const char *message,
		PurpleMessageFlags flags, time_t mtime);

static PurpleCoreUiOps adapterCoreUIOps =
{ NULL, NULL, adapterUIInit, NULL, getClientInfo, NULL, NULL, NULL };

static PurpleEventLoopUiOps adapterEventLoopUIOps =
{ g_timeout_add, g_source_remove, adapterIOAdd, g_source_remove, NULL, g_timeout_add_seconds, NULL, NULL, NULL };

static PurpleConversationUiOps adapterConversationUIOps  =
{ NULL, NULL, NULL, NULL, incoming_message_cb, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL };
