/*
 * <LibpurpleAdapter.c: wrapper around libpurple.so>
 *
 * Copyright 2009 Palm, Inc. All rights reserved.
 *
 * This program is free software and licensed under the terms of the GNU 
 * Lesser General Public License Version 2.1 as published by the Free 
 * Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public License,   
 * Version 2.1 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-   
 * 1301, USA  

 * The LibpurpleAdapter is a wrapper around libpurple.so and provides a simple API over D-Bus to be used by
 * the messaging service and potentially other interested services/apps. 
 * The same D-Bus API will be implemented by other transport providers (e.g. Oz)
 * 
 */

#include "purple.h"

#include <glib.h>

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "defines.h"

#include <cjson/json.h>
#include <lunaservice.h>
#include <json_utils.h>

#include <pthread.h>

#define PURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)
#define CONNECT_TIMEOUT_SECONDS 30

/**
 * The number of seconds we wait before disabling the server queue after the screen turns on
 */
#define DISABLE_QUEUE_TIMEOUT_SECONDS 10

/**
 * The number of seconds we wait after login before we enable the server queue (if display is off)
 */
#define POST_LOGIN_WAIT_SECONDS 10

static const char *dbusAddress = "im.libpurple.palm";

static LSHandle *serviceHandle = NULL;
/**
 * List of accounts that are online
 */
static GHashTable *onlineAccountData = NULL;
/**
 * List of accounts that are in the process of logging in
 */
static GHashTable *pendingAccountData = NULL;
static GHashTable *offlineAccountData = NULL;
static GHashTable *accountLoginTimers = NULL;
static GHashTable *loginMessages = NULL;
static GHashTable *logoutMessages = NULL;
static GHashTable *connectionTypeData = NULL;

static bool libpurpleInitialized = FALSE;
static bool registeredForAccountSignals = FALSE;
static bool registeredForPresenceUpdateSignals = FALSE;
static bool registeredForDisplayEvents = FALSE;
static bool currentDisplayState = TRUE; // TRUE: display on
/** 
 * Keeps track of the local IP address that we bound to when logging in to individual accounts 
 * key: accountKey, value: IP address 
 */
static GHashTable *ipAddressesBoundTo = NULL;

static void adapterUIInit(void)
{
	purple_conversations_set_ui_ops(&adapterConversationUIOps);
}

static void destroyNotify(gpointer dataToFree)
{
	g_free(dataToFree);
}

static gboolean adapterInvokeIO(GIOChannel *ioChannel, GIOCondition ioCondition, gpointer data)
{
	IOClosure *ioClosure = data;
	PurpleInputCondition purpleCondition = 0;
	
	if (PURPLE_GLIB_READ_COND & ioCondition)
	{
		purpleCondition = purpleCondition | PURPLE_INPUT_READ;
	}
	
	if (PURPLE_GLIB_WRITE_COND & ioCondition)
	{
		purpleCondition = purpleCondition | PURPLE_INPUT_WRITE;
	}

	ioClosure->function(ioClosure->data, g_io_channel_unix_get_fd(ioChannel), purpleCondition);

	return TRUE;
}

static guint adapterIOAdd(gint fd, PurpleInputCondition purpleCondition, PurpleInputFunction inputFunction, gpointer data)
{
	GIOChannel *ioChannel;
	GIOCondition ioCondition = 0;
	IOClosure *ioClosure = g_new0(IOClosure, 1);

	ioClosure->data = data;
	ioClosure->function = inputFunction;

	if (PURPLE_INPUT_READ & purpleCondition)
	{
		ioCondition = ioCondition | PURPLE_GLIB_READ_COND;
	}
	
	if (PURPLE_INPUT_WRITE & purpleCondition)
	{
		ioCondition = ioCondition | PURPLE_GLIB_WRITE_COND;
	}

	ioChannel = g_io_channel_unix_new(fd);
	ioClosure->result = g_io_add_watch_full(ioChannel, G_PRIORITY_DEFAULT, ioCondition, adapterInvokeIO, ioClosure,
			destroyNotify);

	g_io_channel_unref(ioChannel);
	return ioClosure->result;
}
/*
 * Helper methods 
 * TODO: move them to the right spot
 */

/*
 * TODO: make constants for the java-side availability values
 */
static int getPalmAvailabilityFromPrplAvailability(int prplAvailability)
{
	switch (prplAvailability)
	{
	case PURPLE_STATUS_UNSET:
		return 6;
	case PURPLE_STATUS_OFFLINE:
		return 4;
	case PURPLE_STATUS_AVAILABLE:
		return 0;
	case PURPLE_STATUS_UNAVAILABLE:
		return 2;
	case PURPLE_STATUS_INVISIBLE:
		return 3;
	case PURPLE_STATUS_AWAY:
		return 2;
	case PURPLE_STATUS_EXTENDED_AWAY:
		return 2;
	case PURPLE_STATUS_MOBILE:
		return 1;
	case PURPLE_STATUS_TUNE:
		return 0;
	default:
		return 4;
	}
}

static int getPrplAvailabilityFromPalmAvailability(int palmAvailability)
{
	switch (palmAvailability)
	{
	case 0:
		return PURPLE_STATUS_AVAILABLE;
	case 1:
		return PURPLE_STATUS_MOBILE;
	case 2:
		return PURPLE_STATUS_AWAY;
	case 3:
		return PURPLE_STATUS_INVISIBLE;
	case 4:
		return PURPLE_STATUS_OFFLINE;
	default:
		return PURPLE_STATUS_OFFLINE;
	}
}

/*
 * This method handles special cases where the username passed by the java side does not satisfy a particular prpl's requirement
 * (e.g. for logging into AIM, the java service uses "amiruci@aol.com", yet the aim prpl expects "amiruci"; same scenario with yahoo)
 * Free the returned string when you're done with it 
 */
static char* getPrplFriendlyUsername(const char *serviceName, const char *username)
{
	if (!username || !serviceName)
	{
		return "";
	}
	char *transportFriendlyUsername;
	if (strcmp(serviceName, "aol") == 0)
	{
		if (strstr(username, "@aol.com") != NULL)
		{
			transportFriendlyUsername = malloc(strlen(username) - strlen("@aol.com") + 1);
			char *usernameCopy= alloca(strlen(username) + 1);
			strcpy(usernameCopy, username);
			strtok(usernameCopy, "@");
			strcpy(transportFriendlyUsername, usernameCopy);
			return transportFriendlyUsername;
		}
	}
	else if (strcmp(serviceName, "yahoo") == 0)
	{
		if (strstr(username, "@yahoo.com") != NULL)
		{
			transportFriendlyUsername = malloc(strlen(username) - strlen("@yahoo.com") + 1);
			char *usernameCopy= alloca(strlen(username) + 1);
			strcpy(usernameCopy, username);
			strtok(usernameCopy, "@");
			strcpy(transportFriendlyUsername, usernameCopy);
			return transportFriendlyUsername;
		}
	}
	transportFriendlyUsername = malloc(strlen(username) + 1);
	transportFriendlyUsername = strcpy(transportFriendlyUsername, username);
	return transportFriendlyUsername;
}

/*
 * The messaging service expects the username to be in the username@domain.com format, whereas the AIM prpl uses the username only
 * Free the returned string when you're done with it 
 */
static char* getJavaFriendlyUsername(const char *username, const char *serviceName)
{
	if (!username || !serviceName)
	{
		return "";
	}
	GString *javaFriendlyUsername = g_string_new(username);
	if (strcmp(serviceName, "aol") == 0 && strchr(username, '@') == NULL)
	{
		g_string_append(javaFriendlyUsername, "@aol.com");
	}
	else if (strcmp(serviceName, "yahoo") == 0 && strchr(username, '@') == NULL)
	{
		g_string_append(javaFriendlyUsername, "@yahoo.com");
	}
	else if (strcmp(serviceName, "gmail") == 0)
	{
		char *resource = memchr(username, '/', strlen(username));
		if (resource != NULL)
		{
			int charsToKeep = resource - username;
			g_string_erase(javaFriendlyUsername, charsToKeep, -1);
		}
	}
	return javaFriendlyUsername->str;
}

static char* stripResourceFromGtalkUsername(const char *username)
{
	if (!username)
	{
		return "";
	}
	GString *javaFriendlyUsername = g_string_new(username);
	char *resource = memchr(username, '/', strlen(username));
	if (resource != NULL)
	{
		int charsToKeep = resource - username;
		g_string_erase(javaFriendlyUsername, charsToKeep, -1);
	}
	return javaFriendlyUsername->str;
}

static char* getJavaFriendlyErrorCode(PurpleConnectionError type)
{
	char *javaFriendlyErrorCode;
	if (type == PURPLE_CONNECTION_ERROR_INVALID_USERNAME)
	{
		javaFriendlyErrorCode = "AcctMgr_Bad_Username";
	}
	else if (type == PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED)
	{
		javaFriendlyErrorCode = "AcctMgr_Bad_Authentication";
	}
	else if (type == PURPLE_CONNECTION_ERROR_NETWORK_ERROR)
	{
		javaFriendlyErrorCode = "AcctMgr_Network_Error";
	}
	else if (type == PURPLE_CONNECTION_ERROR_NAME_IN_USE)
	{
		javaFriendlyErrorCode = "AcctMgr_Name_In_Use";
	}
	else
	{
		syslog(LOG_INFO, "PurpleConnectionError was %i", type);
		javaFriendlyErrorCode = "AcctMgr_Generic_Error";
	}
	return javaFriendlyErrorCode;
}

/*
 * Given java-friendly serviceName, it will return prpl-specific protocol_id (e.g. given "aol", it will return "prpl-aim")
 * Free the returned string when you're done with it 
 */
static char* getPrplProtocolIdFromServiceName(const char *serviceName)
{
	if (!serviceName)
	{
		return "";
	}
	GString *prplProtocolId = g_string_new("prpl-");
	if (strcmp(serviceName, "aol") == 0)
	{
		// Special case for aol where the java serviceName is "aol" and the prpl protocol_id is "aim-prpl"
		// I can imagine that we'll have more of these cases coming up...
		g_string_append(prplProtocolId, "aim");
	}
	else if (strcmp(serviceName, "gmail") == 0)
	{
		// Special case for gtalk where the java serviceName is "gmail" and the prpl protocol_id is "aim-jabber"
		g_string_append(prplProtocolId, "jabber");
	}
	else
	{
		g_string_append(prplProtocolId, serviceName);
	}
	return prplProtocolId->str;
}

/*
 * Given the prpl-specific protocol_id, it will return java-friendly serviceName (e.g. given "prpl-aim", it will return "aol")
 * Free the returned string when you're done with it 
 */
static char* getServiceNameFromPrplProtocolId(char *prplProtocolId)
{
	if (!prplProtocolId)
	{
		return "";
	}
	char *stringChopper = prplProtocolId;
	stringChopper += strlen("prpl-");
	GString *serviceName = g_string_new(stringChopper);

	if (strcmp(serviceName->str, "aim") == 0)
	{
		// Special case for aol where the java serviceName is "aol" and the prpl protocol_id is "aim-prpl"
		// I can imagine that we'll have more of these cases coming up...
		serviceName = g_string_new("aol");
	}
	else if (strcmp(serviceName->str, "jabber") == 0)
	{
		// Special case for gtalk where the java serviceName is "gmail" and the prpl protocol_id is "jabber-purple"
		serviceName = g_string_new("gmail");
	}
	return serviceName->str;
}

static char* getAccountKey(const char *username, const char *serviceName)
{
	if (!username || !serviceName)
	{
		return "";
	}
	char *accountKey = malloc(strlen(username) + strlen(serviceName) + 2);
	strcpy(accountKey, username);
	strcat(accountKey, "_");
	strcat(accountKey, serviceName);
	return accountKey;
}

static char* getAccountKeyFromPurpleAccount(PurpleAccount *account)
{
	if (!account)
	{
		return "";
	}
	char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
	char *username = getJavaFriendlyUsername(account->username, serviceName);
	char *accountKey = getAccountKey(username, serviceName);

	free(serviceName);
	free(username);

	return accountKey;
}

static const char* getField(struct json_object* message, const char* name)
{
	struct json_object* val = json_object_object_get(message, name);
	if (val)
	{
		return json_object_get_string(val);
	}
	return NULL;
}

/**
 * Returns a GString containing the special stanza to enable server-side presence update queue
 * Clean up after yourself using g_string_free when you're done with the return value
 */
static GString* getEnableQueueStanza(PurpleAccount *account)
{
	GString *stanza = NULL;
	if (account != NULL)
	{
		stanza = g_string_new("");
		PurpleConnection *pc = purple_account_get_connection(account);
		if (pc == NULL)
		{
			return NULL;
		}
		const char *displayName = purple_connection_get_display_name(pc);
		if (displayName == NULL)
		{
			return NULL;
		}
		g_string_append(stanza, "<iq from='");
		g_string_append(stanza, displayName);
		g_string_append(stanza, "' type='set'><query xmlns='google:queue'><enable/></query></iq>");
	}
	return stanza;
}

/**
 * Returns a GString containing the special stanza to disable and flush the server-side presence update queue
 * Clean up after yourself using g_string_free when you're done with the return value
 */
static GString* getDisableQueueStanza(PurpleAccount *account)
{
	GString *stanza = NULL;
	if (account != NULL)
	{
		stanza = g_string_new("");
		PurpleConnection *pc = purple_account_get_connection(account);
		if (pc == NULL)
		{
			return NULL;
		}
		const char *displayName = purple_connection_get_display_name(pc);
		if (displayName == NULL)
		{
			return NULL;
		}
		g_string_append(stanza, "<iq from='");
		g_string_append(stanza, displayName);
		g_string_append(stanza, "' type='set'><query xmlns='google:queue'><disable/><flush/></query></iq>");
	}
	return stanza;
}

static void enableServerQueueForAccount(PurpleAccount *account)
{
	if (!account)
	{
		return;
	}

	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	{
		prpl = purple_connection_get_prpl(gc);
	}

	if (prpl != NULL)
	{
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	}

	if (prpl_info && prpl_info->send_raw)
	{
		GString *enableQueueStanza = getEnableQueueStanza(account);
		if (enableQueueStanza != NULL) 
		{
			syslog(LOG_INFO, "Enabling server queue");
			prpl_info->send_raw(gc, enableQueueStanza->str, enableQueueStanza->len);
			g_string_free(enableQueueStanza, TRUE);
		}
	}
}

static void disableServerQueueForAccount(PurpleAccount *account)
{
	if (!account)
	{
		return;
	}
	PurplePluginProtocolInfo *prpl_info = NULL;
	PurpleConnection *gc = purple_account_get_connection(account);
	PurplePlugin *prpl = NULL;
	
	if (gc != NULL)
	{
		prpl = purple_connection_get_prpl(gc);
	}

	if (prpl != NULL)
	{
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	}

	if (prpl_info && prpl_info->send_raw)
	{
		GString *disableQueueStanza = getDisableQueueStanza(account);
		if (disableQueueStanza != NULL) 
		{
			syslog(LOG_INFO, "Disabling server queue");
			prpl_info->send_raw(gc, disableQueueStanza->str, disableQueueStanza->len);
			g_string_free(disableQueueStanza, TRUE);
		}
	}
}

/**
 * Asking the gtalk server to enable/disable queueing of presence updates 
 * This is called when the screen is turned off (enalbe:true) or turned on (enable:false)
 */
static bool queuePresenceUpdates(bool enable)
{
	bool retVal;
	bool success = TRUE;
	PurpleAccount *account;
	GList *onlineAccountKeys = NULL;
	GList *iterator = NULL;
	char *accountKey = "";
	
	onlineAccountKeys = g_hash_table_get_keys(onlineAccountData);
	for (iterator = onlineAccountKeys; iterator != NULL; iterator = g_list_next(iterator))
	{
		accountKey = iterator->data;
		account = g_hash_table_lookup(onlineAccountData, accountKey);
		if (!account || strcmp(account->protocol_id, "prpl-jabber") != 0)
		{
			/*
			 * enabling/disabling server queue is only supported by gtalk
			 */
			continue;
		}
		if (enable)
		{
			enableServerQueueForAccount(account);
		}
		else
		{
			disableServerQueueForAccount(account);
		}
	}
	return TRUE;
}

static gboolean queuePresenceUpdatesTimer(gpointer data)
{
	if (currentDisplayState)
	{
		queuePresenceUpdates(FALSE);
	}
	return FALSE;
}

static gboolean queuePresenceUpdatesForAccountTimerCallback(gpointer data)
{
	/*
	 * if the display is still off, then enable the server queue for this account
	 */
	if (!currentDisplayState)
	{
		char *accountKey = data;
		PurpleAccount *account = g_hash_table_lookup(onlineAccountData, accountKey);
		if (account != NULL)
		{
			enableServerQueueForAccount(account);
		}
	}
	return FALSE;
}

static void respondWithFullBuddyList(PurpleAccount *account, char *serviceName, char *myJavaFriendlyUsername)
{
	if (!account || !myJavaFriendlyUsername || !serviceName)
	{
		syslog(LOG_INFO, "ERROR: respondWithFullBuddyList was passed NULL");
		return;
	}
	GSList *buddyList = purple_find_buddies(account, NULL);
	if (!buddyList)
	{
		syslog(LOG_INFO, "ERROR: the buddy list was NULL");
	}

	GString *jsonResponse = g_string_new("{\"serviceName\":\"");
	g_string_append(jsonResponse, serviceName);
	g_string_append(jsonResponse, "\", \"username\":\"");
	g_string_append(jsonResponse, myJavaFriendlyUsername);
	g_string_append(jsonResponse, "\", \"fullBuddyList\":true, \"buddies\":[");

	GSList *buddyIterator = NULL;
	PurpleBuddy *buddyToBeAdded = NULL;
	PurpleGroup *group = NULL;

	printf("\n\n\n\n\n\n ---------------- BUDDY LIST SIZE: %d----------------- \n\n\n\n\n\n", g_slist_length(buddyList));

	bool firstItem = TRUE;

	for (buddyIterator = buddyList; buddyIterator != NULL; buddyIterator = buddyIterator->next)
	{
		buddyToBeAdded = (PurpleBuddy *) buddyIterator->data;
		group = purple_buddy_get_group(buddyToBeAdded);
		const char *groupName = purple_group_get_name(group);

		if (buddyToBeAdded->alias == NULL)
		{
			buddyToBeAdded->alias = "";
		}

		/*
		 * Getting the availability
		 */
		PurpleStatus *status = purple_presence_get_active_status(purple_buddy_get_presence(buddyToBeAdded));
		int newStatusPrimitive = purple_status_type_get_primitive(purple_status_get_type(status));
		int availabilityInt = getPalmAvailabilityFromPrplAvailability(newStatusPrimitive);
		char availabilityString[2];
		sprintf(availabilityString, "%i", availabilityInt);

		/*
		 * Getting the custom message
		 */
		const char *customMessage = purple_status_get_attr_string(status, "message");
		if (customMessage == NULL)
		{
			customMessage = "";
		}

		/*
		 * Getting the avatar location
		 */
		PurpleBuddyIcon *icon = purple_buddy_get_icon(buddyToBeAdded);
		char* buddyAvatarLocation = NULL;
		if (icon != NULL)
		{
			buddyAvatarLocation = purple_buddy_icon_get_full_path(icon);
		}

		if (!firstItem)
		{
			g_string_append(jsonResponse, ", ");
		}
		else
		{
			firstItem = FALSE;
		}

		struct json_object *payload = json_object_new_object();
		json_object_object_add(payload, "buddyUsername", json_object_new_string(buddyToBeAdded->name));
		json_object_object_add(payload, "displayName", json_object_new_string(buddyToBeAdded->alias));
		json_object_object_add(payload, "avatarLocation", json_object_new_string((buddyAvatarLocation) ? buddyAvatarLocation : ""));
		json_object_object_add(payload, "customMessage", json_object_new_string((char*)customMessage));
		json_object_object_add(payload, "availability", json_object_new_string(availabilityString));
		json_object_object_add(payload, "groupName", json_object_new_string((char*)groupName));
		g_string_append(jsonResponse, json_object_to_json_string(payload));
		g_message(
				"%s says: %s's presence: availability: '%s', custom message: '%s', avatar location: '%s', display name: '%s', group name:'%s'",
				__FUNCTION__, buddyToBeAdded->name, availabilityString, customMessage, buddyAvatarLocation,
				buddyToBeAdded->alias, groupName);
		if (!is_error(payload)) 
		{
			json_object_put(payload);
		}
		if (buddyAvatarLocation)
		{
			g_free(buddyAvatarLocation);
		}
	}
	g_string_append(jsonResponse, "]}");
	LSError lserror;
	LSErrorInit(&lserror);
	bool retVal = LSSubscriptionReply(serviceHandle, "/getBuddyList", jsonResponse->str, &lserror);
	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}
	LSErrorFree(&lserror);
	g_string_free(jsonResponse, TRUE);
}

/*
 * End of helper methods 
 */

/*
 * Callbacks
 */

static void buddy_signed_on_off_cb(PurpleBuddy *buddy, gpointer data)
{
	gboolean signed_on = GPOINTER_TO_INT(data);

	LSError lserror;
	LSErrorInit(&lserror);

	PurpleAccount *account = purple_buddy_get_account(buddy);
	char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
	const char *myUsername = purple_account_get_username(account);
	char *myJavaFriendlyUsername = getJavaFriendlyUsername(myUsername, serviceName);
	PurpleStatus *activeStatus = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
	/*
	 * Getting the new availability
	 */
	int newStatusPrimitive = purple_status_type_get_primitive(purple_status_get_type(activeStatus));
	int newAvailabilityValue = getPalmAvailabilityFromPrplAvailability(newStatusPrimitive);
	char availabilityString[2];
	PurpleBuddyIcon *icon = purple_buddy_get_icon(buddy);
	const char *customMessage = "";
	char* buddyAvatarLocation = NULL;

	sprintf(availabilityString, "%i", newAvailabilityValue);

	if (icon != NULL)
	{
		buddyAvatarLocation = purple_buddy_icon_get_full_path(icon);
	}
		
	if (buddy->name == NULL)
	{
		buddy->name = "";
	}
	
	if (buddy->alias == NULL)
	{
		buddy->alias = "";
	}

	customMessage = purple_status_get_attr_string(activeStatus, "message");
	if (customMessage == NULL)
	{
		customMessage = "";
	}

	PurpleGroup *group = purple_buddy_get_group(buddy);
	const char *groupName = purple_group_get_name(group);
	if (groupName == NULL)
	{
		groupName = "";
	}
	
	struct json_object *payload = json_object_new_object();
	json_object_object_add(payload, "serviceName", json_object_new_string(serviceName));
	json_object_object_add(payload, "username", json_object_new_string(myJavaFriendlyUsername));
	json_object_object_add(payload, "buddyUsername", json_object_new_string(buddy->name));
	json_object_object_add(payload, "displayName", json_object_new_string(buddy->alias));
	json_object_object_add(payload, "avatarLocation", json_object_new_string((buddyAvatarLocation) ? buddyAvatarLocation : ""));
	json_object_object_add(payload, "customMessage", json_object_new_string((char*)customMessage));
	json_object_object_add(payload, "availability", json_object_new_string(availabilityString));
	json_object_object_add(payload, "groupName", json_object_new_string((char*)groupName));

	bool retVal = LSSubscriptionReply(serviceHandle, "/getBuddyList", json_object_to_json_string(payload), &lserror);
	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}
	LSErrorFree(&lserror);
	
	g_message(
			"%s says: %s's presence: availability: '%s', custom message: '%s', avatar location: '%s', display name: '%s', group name: '%s'",
			__FUNCTION__, buddy->name, availabilityString, customMessage, buddyAvatarLocation, buddy->alias, groupName);
	
	if (myJavaFriendlyUsername)
	{
		free(myJavaFriendlyUsername);
	}
	if (!is_error(payload)) 
	{
		json_object_put(payload);
	}	
	if (buddyAvatarLocation)
	{
		g_free(buddyAvatarLocation);
	}
}

static void buddy_status_changed_cb(PurpleBuddy *buddy, PurpleStatus *old_status, PurpleStatus *new_status,
		gpointer unused)
{
	/*
	 * Getting the new availability
	 */
	int newStatusPrimitive = purple_status_type_get_primitive(purple_status_get_type(new_status));
	int newAvailabilityValue = getPalmAvailabilityFromPrplAvailability(newStatusPrimitive);
	char availabilityString[2];
	sprintf(availabilityString, "%i", newAvailabilityValue);

	/*
	 * Getting the new custom message
	 */
	const char *customMessage = purple_status_get_attr_string(new_status, "message");
	if (customMessage == NULL)
	{
		customMessage = "";
	}

	LSError lserror;
	LSErrorInit(&lserror);

	PurpleAccount *account = purple_buddy_get_account(buddy);
	char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
	char *username = getJavaFriendlyUsername(account->username, serviceName);

	PurpleBuddyIcon *icon = purple_buddy_get_icon(buddy);
	char* buddyAvatarLocation = NULL;	
	if (icon != NULL)
	{
		buddyAvatarLocation = purple_buddy_icon_get_full_path(icon);
	}

	PurpleGroup *group = purple_buddy_get_group(buddy);
	const char *groupName = purple_group_get_name(group);
	if (groupName == NULL)
	{
		groupName = "";
	}

	char *buddyName = buddy->name;
	if (buddyName == NULL)
	{
		buddyName = "";
	}

	struct json_object *payload = json_object_new_object();
	json_object_object_add(payload, "serviceName", json_object_new_string(serviceName));
	json_object_object_add(payload, "username", json_object_new_string(username));
	json_object_object_add(payload, "buddyUsername", json_object_new_string(buddyName));
	json_object_object_add(payload, "avatarLocation", json_object_new_string((buddyAvatarLocation) ? buddyAvatarLocation : ""));
	json_object_object_add(payload, "customMessage", json_object_new_string((char*)customMessage));
	json_object_object_add(payload, "availability", json_object_new_string(availabilityString));
	json_object_object_add(payload, "groupName", json_object_new_string((char*)groupName));

	bool retVal = LSSubscriptionReply(serviceHandle, "/getBuddyList", json_object_to_json_string(payload), &lserror);
	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}
	LSErrorFree(&lserror);
	
	g_message(
			"%s says: %s's presence: availability: '%s', custom message: '%s', avatar location: '%s', display name: '%s', group name: '%s'",
			__FUNCTION__, buddy->name, availabilityString, customMessage, buddyAvatarLocation, buddy->alias, groupName);
	
	if (serviceName)
	{
		free(serviceName);
	}
	if (username)
	{
		free(username);
	}
	if (!is_error(payload)) 
	{
		json_object_put(payload);
	}
	if (buddyAvatarLocation)
	{
		g_free(buddyAvatarLocation);
	}
}

static void buddy_avatar_changed_cb(PurpleBuddy *buddy)
{
	PurpleStatus *activeStatus = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
	buddy_status_changed_cb(buddy, activeStatus, activeStatus, NULL);
}

static bool displayEventHandler(LSHandle *sh , LSMessage *message, void *ctx)
{
    const char *payload = LSMessageGetPayload(message);
    struct json_object *params = json_tokener_parse(payload);
    if (is_error(params)) goto end;
    
    const char *displayState = NULL;
    bool newDisplayState = currentDisplayState;
    bool returnValue = json_object_get_boolean(json_object_object_get(params, "returnValue"));
    
    if (returnValue)
    {
    	displayState = getField(params, "state");
		if (displayState && strcmp(displayState, "on") == 0)
		{
			newDisplayState = TRUE;
		}
		else if (displayState && strcmp(displayState, "off") == 0)
		{
			newDisplayState = FALSE;
		}
		else
		{
			displayState = getField(params, "event");
			if (displayState && strcmp(displayState, "displayOn") == 0)
			{
				newDisplayState = TRUE;
			}
			else if (displayState && strcmp(displayState, "displayOff") == 0)
			{
				newDisplayState = FALSE;
			}
			else
			{
				goto end;
			}
		}
		if (newDisplayState != currentDisplayState)
		{
			currentDisplayState = newDisplayState;
			if (currentDisplayState)
			{
				/*
				 * display has turned on, therefore we disable and flush the queue (after DISABLE_QUEUE_TIMEOUT_SECONDS seconds for perf reasons)
				 */
				purple_timeout_add_seconds(DISABLE_QUEUE_TIMEOUT_SECONDS, queuePresenceUpdatesTimer, NULL);
			}
			else
			{
				/*
				 * display has turned off, therefore we enable the queue
				 */
				queuePresenceUpdates(TRUE);
			}
		}
    }
    else 
    {
    	currentDisplayState = TRUE;
    	registeredForDisplayEvents = FALSE;
    	queuePresenceUpdates(FALSE);
    }

end:
    if (!is_error(params)) json_object_put(params);
    return TRUE;
}

static void account_logged_in(PurpleConnection *gc, gpointer unused)
{
	void *blist_handle = purple_blist_get_handle();
	static int handle;

	PurpleAccount *loggedInAccount = purple_connection_get_account(gc);
	g_return_if_fail(loggedInAccount != NULL);

	char *accountKey = getAccountKeyFromPurpleAccount(loggedInAccount);

	if (g_hash_table_lookup(onlineAccountData, accountKey) != NULL)
	{
		//TODO: we were online. why are we getting notified that we're connected again? we were never disconnected.
		return;
	}

	/*
	 * cancel the connect timeout for this account
	 */
	guint timerHandle = (guint)g_hash_table_lookup(accountLoginTimers, accountKey);

	purple_timeout_remove(timerHandle);
	g_hash_table_remove(accountLoginTimers, accountKey);

	g_hash_table_insert(onlineAccountData, accountKey, loggedInAccount);
	g_hash_table_remove(pendingAccountData, accountKey);

	syslog(LOG_INFO, "Account connected...");

	char *serviceName = getServiceNameFromPrplProtocolId(loggedInAccount->protocol_id);
	char *myJavaFriendlyUsername = getJavaFriendlyUsername(loggedInAccount->username, serviceName);

	GString *jsonResponse = g_string_new("{\"serviceName\":\"");
	g_string_append(jsonResponse, serviceName);
	g_string_append(jsonResponse, "\",  \"username\":\"");
	g_string_append(jsonResponse, myJavaFriendlyUsername);
	g_string_append(jsonResponse, "\", \"returnValue\":true}");

	LSError lserror;
	LSErrorInit(&lserror);

	LSMessage *message = g_hash_table_lookup(loginMessages, getAccountKeyFromPurpleAccount(loggedInAccount));
	bool retVal = LSMessageReply(serviceHandle, message, jsonResponse->str, &lserror);

	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}

	if (registeredForPresenceUpdateSignals == FALSE)
	{
		purple_signal_connect(blist_handle, "buddy-status-changed", &handle, PURPLE_CALLBACK(buddy_status_changed_cb),
				NULL);
		purple_signal_connect(blist_handle, "buddy-signed-on", &handle, PURPLE_CALLBACK(buddy_signed_on_off_cb),
				GINT_TO_POINTER(TRUE));
		purple_signal_connect(blist_handle, "buddy-signed-off", &handle, PURPLE_CALLBACK(buddy_signed_on_off_cb),
				GINT_TO_POINTER(FALSE));
		purple_signal_connect(blist_handle, "buddy-icon-changed", &handle, PURPLE_CALLBACK(buddy_avatar_changed_cb),
				GINT_TO_POINTER(FALSE));
		registeredForPresenceUpdateSignals = TRUE;
	}
	
	if (registeredForDisplayEvents == FALSE)
	{
		retVal = LSCall(serviceHandle, "luna://com.palm.display/control/status",
		        "{\"subscribe\":true}", displayEventHandler, NULL, NULL, &lserror);
		if (!retVal) goto error;
		registeredForDisplayEvents = TRUE;
	}
	else
	{
		/*
		 * This account has just been logged in. We should enable queuing of presence updates 
		 * if the screen is off, but not until we get the initial presence updates
		 */
		if (currentDisplayState == FALSE)
		{
			guint handle = purple_timeout_add_seconds(POST_LOGIN_WAIT_SECONDS, queuePresenceUpdatesForAccountTimerCallback, accountKey);
			if (!handle)
			{
				syslog(LOG_INFO, "purple_timeout_add_seconds failed in account_logged_in");
			}
		}
	}
	
error:
	LSErrorFree(&lserror);
	g_string_free(jsonResponse, TRUE);
}

static void account_signed_off_cb(PurpleConnection *gc, void *data)
{
	syslog(LOG_INFO, "account_signed_off_cb");

	PurpleAccount *account = purple_connection_get_account(gc);
	g_return_if_fail(account != NULL);

	char *accountKey = getAccountKeyFromPurpleAccount(account);
	if (g_hash_table_lookup(onlineAccountData, accountKey) != NULL)
	{
		g_hash_table_remove(onlineAccountData, accountKey);
	}
	else if (g_hash_table_lookup(pendingAccountData, accountKey) != NULL)
	{
		g_hash_table_remove(pendingAccountData, accountKey);
	}
	else
	{
		return;
	}
	g_hash_table_remove(ipAddressesBoundTo, accountKey);
	//g_hash_table_remove(connectionTypeData, accountKey);

	syslog(LOG_INFO, "Account disconnected...");

	if (g_hash_table_lookup(offlineAccountData, accountKey) == NULL)
	{
		/*
		 * Keep the PurpleAccount struct to reuse in future logins
		 */
		g_hash_table_insert(offlineAccountData, accountKey, account);
	}
	
	LSMessage *message = g_hash_table_lookup(logoutMessages, accountKey);
	if (message != NULL)
	{
		char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
		char *myJavaFriendlyUsername = getJavaFriendlyUsername(account->username, serviceName);

		GString *jsonResponse = g_string_new("{\"serviceName\":\"");
		g_string_append(jsonResponse, serviceName);
		g_string_append(jsonResponse, "\",  \"username\":\"");
		g_string_append(jsonResponse, myJavaFriendlyUsername);
		g_string_append(jsonResponse, "\", \"returnValue\":true}");

		LSError lserror;
		LSErrorInit(&lserror);

		bool retVal = LSMessageReply(serviceHandle, message, jsonResponse->str, &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		g_hash_table_remove(logoutMessages, accountKey);
		LSMessageUnref(message);
		LSErrorFree(&lserror);
		g_string_free(jsonResponse, TRUE);
	}
}

/*
 * This callback is called if a) the login attempt failed, or b) login was successful but the session was closed 
 * (e.g. connection problems, etc).
 */
static void account_login_failed(PurpleConnection *gc, PurpleConnectionError type, const gchar *description,
		gpointer unused)
{
	syslog(LOG_INFO, "account_login_failed is called with description %s", description);

	PurpleAccount *account = purple_connection_get_account(gc);
	g_return_if_fail(account != NULL);

	gboolean loggedOut = FALSE;
	char *accountKey = getAccountKeyFromPurpleAccount(account);
	if (g_hash_table_lookup(onlineAccountData, accountKey) != NULL)
	{
		/* 
		 * We were online on this account and are now disconnected because either a) the data connection is dropped, 
		 * b) the server is down, or c) the user has logged in from a different location and forced this session to
		 * get closed.
		 */
		loggedOut = TRUE;
	}
	else
	{
		/*
		 * cancel the connect timeout for this account
		 */
		guint timerHandle = (guint)g_hash_table_lookup(accountLoginTimers, accountKey);
		purple_timeout_remove(timerHandle);
		g_hash_table_remove(accountLoginTimers, accountKey);

		if (g_hash_table_lookup(pendingAccountData, accountKey) == NULL)
		{
			/*
			 * This account was in neither of the account data lists (online or pending). We must have logged it out 
			 * and not cared about letting java know about it (probably because java went down and came back up and 
			 * thought that the account was logged out anyways)
			 */
			return;
		}
		else
		{
			g_hash_table_remove(pendingAccountData, accountKey);
		}
	}

	char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
	char *myJavaFriendlyUsername = getJavaFriendlyUsername(account->username, serviceName);
	char *javaFriendlyErrorCode = getJavaFriendlyErrorCode(type);
	char *accountBoundToIpAddress = g_hash_table_lookup(ipAddressesBoundTo, accountKey);
	char *connectionType = g_hash_table_lookup(connectionTypeData, accountKey);

	if (accountBoundToIpAddress == NULL)
	{
		accountBoundToIpAddress = "";
	}

	if (connectionType == NULL)
	{
		connectionType = "";
	}

	GString *jsonResponse = g_string_new("{\"serviceName\":\"");
	g_string_append(jsonResponse, serviceName);
	g_string_append(jsonResponse, "\",  \"username\":\"");
	g_string_append(jsonResponse, myJavaFriendlyUsername);
	g_string_append(jsonResponse, "\", \"returnValue\":false, \"errorCode\":\"");
	g_string_append(jsonResponse, javaFriendlyErrorCode);
	g_string_append(jsonResponse, "\",  \"localIpAddress\":\"");
	g_string_append(jsonResponse, accountBoundToIpAddress);
	g_string_append(jsonResponse, "\", \"errorText\":\"");
	g_string_append(jsonResponse, g_strescape(description, NULL));
	if (loggedOut)
	{
		g_string_append(jsonResponse, "\", \"connectionStatus\":\"loggedOut\", \"connectionType\":\"");
		g_string_append(jsonResponse, connectionType);
		g_string_append(jsonResponse, "\"}");
		syslog(LOG_INFO, "We were logged out. Reason: %s, prpl error code: %i", description, type);
	}
	else
	{
		g_string_append(jsonResponse, "\", \"connectionType\":\"");
		g_string_append(jsonResponse, connectionType);
		g_string_append(jsonResponse, "\"}");
		syslog(LOG_INFO, "Login failed. Reason: \"%s\", prpl error code: %i", description, type);
	}
	g_hash_table_remove(onlineAccountData, accountKey);
	g_hash_table_remove(ipAddressesBoundTo, accountKey);
	g_hash_table_remove(connectionTypeData, accountKey);
 
	if (g_hash_table_lookup(offlineAccountData, accountKey) == NULL)
	{
		/*
		 * Keep the PurpleAccount struct to reuse in future logins
		 */
		g_hash_table_insert(offlineAccountData, accountKey, account);
	}
	
	LSError lserror;
	LSErrorInit(&lserror);

	LSMessage *message = g_hash_table_lookup(loginMessages, accountKey);
	if (message != NULL)
	{
		bool retVal = LSMessageReply(serviceHandle, message, jsonResponse->str, &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		g_hash_table_remove(loginMessages, accountKey);
		LSMessageUnref(message);
	}
	LSErrorFree(&lserror);
	g_string_free(jsonResponse, TRUE);
	//free(accountKey);
}

static void account_status_changed(PurpleAccount *account, PurpleStatus *old, PurpleStatus *new, gpointer data)
{
	printf("\n\n ACCOUNT STATUS CHANGED \n\n");
}

static void incoming_message_cb(PurpleConversation *conv, const char *who, const char *alias, const char *message,
		PurpleMessageFlags flags, time_t mtime)
{
	/*
	 * snippet taken from nullclient
	 */
	const char *usernameFrom;
	if (who && *who)
		usernameFrom = who;
	else if (alias && *alias)
		usernameFrom = alias;
	else
		usernameFrom = "";

	if ((flags & PURPLE_MESSAGE_RECV) != PURPLE_MESSAGE_RECV)
	{
		/* this is a sent message. ignore it. */
		return;
	}

	PurpleAccount *account = purple_conversation_get_account(conv);

	char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
	char *username = getJavaFriendlyUsername(account->username, serviceName);

	if (strcmp(username, usernameFrom) == 0)
	{
		/* We get notified even though we sent the message. Just ignore it */
		return;
	}

	if (strcmp(serviceName, "aol") == 0 && (strcmp(usernameFrom, "aolsystemmsg") == 0 || strcmp(usernameFrom,
			"AOL System Msg") == 0))
	{
		/*
		 * ignore messages from the annoying aolsystemmsg telling us that we're logged in somewhere else
		 */
		return;
	}

	char *usernameFromStripped = stripResourceFromGtalkUsername(usernameFrom);

	LSError lserror;
	LSErrorInit(&lserror);

	struct json_object *payload = json_object_new_object();
	json_object_object_add(payload, "serviceName", json_object_new_string(serviceName));
	json_object_object_add(payload, "username", json_object_new_string(username));
	json_object_object_add(payload, "usernameFrom", json_object_new_string(usernameFromStripped));
	json_object_object_add(payload, "messageText", json_object_new_string((char*)message));

	bool retVal = LSSubscriptionReply(serviceHandle, "/registerForIncomingMessages",
			json_object_to_json_string(payload), &lserror);
	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}

	LSErrorFree(&lserror);
	if (serviceName)
	{
		free(serviceName);
	}
	if (username)
	{
		free(username);
	}
	if (usernameFromStripped)
	{
		free(usernameFromStripped);
	}
	if (!is_error(payload)) 
	{
		json_object_put(payload);
	}
}

static gboolean connectTimeoutCallback(gpointer data)
{
	char *accountKey = data;
	PurpleAccount *account = g_hash_table_lookup(pendingAccountData, accountKey);
	if (account == NULL)
	{
		/*
		 * If the account is not pending anymore (which means login either already failed or succeeded) 
		 * then we shouldn't have gotten to this point since we should have cancelled the timer
		 */
		syslog(LOG_INFO,
				"WARNING: we shouldn't have gotten to connectTimeoutCallback since login had already failed/succeeded");
		return FALSE;
	}

	guint timerHandle = (guint)g_hash_table_lookup(accountLoginTimers, accountKey);

	/*
	 * abort logging in since our connect timeout has hit before login either failed or succeeded
	 */
	g_hash_table_remove(accountLoginTimers, accountKey);
	g_hash_table_remove(pendingAccountData, accountKey);
	g_hash_table_remove(ipAddressesBoundTo, accountKey);

	purple_account_disconnect(account);

	char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
	char *username = getJavaFriendlyUsername(account->username, serviceName);
	char *connectionType = g_hash_table_lookup(connectionTypeData, accountKey);
	if (connectionType == NULL)
	{
		connectionType = "";
	}

	GString *jsonResponse = g_string_new("{\"serviceName\":\"");
	g_string_append(jsonResponse, serviceName);
	g_string_append(jsonResponse, "\",  \"username\":\"");
	g_string_append(jsonResponse, username);
	g_string_append(
			jsonResponse,
			"\", \"returnValue\":false, \"errorCode\":\"AcctMgr_Network_Error\", \"errorText\":\"Connection timed out\", \"connectionType\":\"");
	g_string_append(jsonResponse, connectionType);
	g_string_append(jsonResponse, "\"}");

	LSError lserror;
	LSErrorInit(&lserror);

	LSMessage *message = g_hash_table_lookup(loginMessages, accountKey);
	if (message != NULL)
	{
		bool retVal = LSMessageReply(serviceHandle, message, jsonResponse->str, &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		g_hash_table_remove(loginMessages, accountKey);
		LSMessageUnref(message);
	}
	LSErrorFree(&lserror);
	free(serviceName);
	free(username);
	free(accountKey);
	g_string_free(jsonResponse, TRUE);
	return FALSE;
}

/*
 * End of callbacks
 */

/*
 * libpurple initialization methods
 */

static GHashTable* getClientInfo(void)
{
	GHashTable *clientInfo = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	g_hash_table_insert(clientInfo, "name", "Palm Messaging");
	g_hash_table_insert(clientInfo, "version", "");

	return clientInfo;
}

static void initializeLibpurple()
{
	signal(SIGCHLD, SIG_IGN);

	/* Set a custom user directory (optional) */
	purple_util_set_user_dir(CUSTOM_USER_DIRECTORY);

	/* We do not want any debugging for now to keep the noise to a minimum. */
	purple_debug_set_enabled(TRUE);

	/* Set the core-uiops, which is used to
	 * 	- initialize the ui specific preferences.
	 * 	- initialize the debug ui.
	 * 	- initialize the ui components for all the modules.
	 * 	- uninitialize the ui components for all the modules when the core terminates.
	 */
	purple_core_set_ui_ops(&adapterCoreUIOps);

	purple_eventloop_set_ui_ops(&adapterEventLoopUIOps);

	/* purple_core_init () calls purple_dbus_init ().  We don't want libpurple's
	 * own dbus server, so let's kill it here.  Ideally, it would never be
	 * initialized in the first place, but hey.
	 */
	// building without dbus... no need to call this method
	// purple_dbus_uninit();

	if (!purple_core_init(UI_ID))
	{
		syslog(LOG_INFO, "libpurple initialization failed.");
		abort();
	}

	/* Create and load the buddylist. */
	purple_set_blist(purple_blist_new());
	purple_blist_load();

	purple_buddy_icons_set_cache_dir("/var/luna/data/im-avatars");

	libpurpleInitialized = TRUE;
	syslog(LOG_INFO, "libpurple initialized.\n");
}
/*
 * End of libpurple initialization methods
 */

/*
 * Service methods
 */
static bool login(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	const char *serviceName = NULL;
	const char *username = NULL;
	const char *password = NULL;
	int availability = 0;
	const char *customMessage = NULL;
	const char *localIpAddress = NULL;
	const char *connectionType = NULL;

	bool subscribe = FALSE;

	PurpleAccount *account;
	char *myJavaFriendlyUsername = NULL;
	char *prplProtocolId = NULL;
	char *transportFriendlyUserName = NULL;
	char *accountKey = NULL;
	bool accountIsAlreadyOnline = FALSE;
	bool accountIsAlreadyPending = FALSE;

	boolean invalidParameters = TRUE;

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	char *payload = strdup(LSMessageGetPayload(message));

	if (!payload)
	{
		success = FALSE;
		goto error;
	}

	struct json_object *params = json_tokener_parse(payload);
	if (is_error(params))
	{
		success = FALSE;
		goto error;
	}
	subscribe = json_object_get_boolean(json_object_object_get(params, "subscribe"));

	serviceName = getField(params, "serviceName");
	if (!serviceName)
	{
		success = FALSE;
		goto error;
	}

	username = getField(params, "username");
	if (!username)
	{
		success = FALSE;
		goto error;
	}

	password = getField(params, "password");
	if (!password)
	{
		success = FALSE;
		goto error;
	}

	availability = json_object_get_int(json_object_object_get(params, "availability"));

	customMessage = getField(params, "customMessage");
	if (!customMessage)
	{
		customMessage = "";
	}

	localIpAddress = getField(params, "localIpAddress");
	if (!localIpAddress)
	{
		localIpAddress = "";
	}
	else
	{
		localIpAddress = strdup(localIpAddress);
	}
	
	connectionType = getField(params, "connectionType");
	if (!connectionType)
	{
		connectionType = "";
	}
	else
	{
		connectionType = strdup(connectionType);
	}

	invalidParameters = FALSE;

	syslog(LOG_INFO, "Parameters: servicename %s, connectionType %s", serviceName, connectionType);

	if (libpurpleInitialized == FALSE)
	{
		initializeLibpurple();
	}

	/* libpurple variables */
	prplProtocolId = getPrplProtocolIdFromServiceName(serviceName);
	transportFriendlyUserName = getPrplFriendlyUsername(serviceName, username);
	accountKey = getAccountKey(username, serviceName);

	myJavaFriendlyUsername = getJavaFriendlyUsername(username, serviceName);

	struct json_object *responsePayload = json_object_new_object();
	json_object_object_add(responsePayload, "serviceName", json_object_new_string((char*)serviceName));
	json_object_object_add(responsePayload, "username", json_object_new_string((char*)myJavaFriendlyUsername));

	/*
	 * Let's check to see if we're already logged in to this account or that we're already in the process of logging in 
	 * to this account. This can happen when java goes down and comes back up.
	 */
	PurpleAccount *alreadyActiveAccount = g_hash_table_lookup(onlineAccountData, accountKey);
	if (alreadyActiveAccount != NULL)
	{
		accountIsAlreadyOnline = TRUE;
	}
	else
	{
		alreadyActiveAccount = g_hash_table_lookup(pendingAccountData, accountKey);
		if (alreadyActiveAccount != NULL)
		{
			accountIsAlreadyPending = TRUE;
		}
	}

	if (alreadyActiveAccount != NULL)
	{
		/*
		 * We're either already logged in to this account or we're already in the process of logging in to this account 
		 * (i.e. it's pending; waiting for server response)
		 */
		char *accountBoundToIpAddress = g_hash_table_lookup(ipAddressesBoundTo, accountKey);
		if (accountBoundToIpAddress != NULL && strcmp(localIpAddress, accountBoundToIpAddress) == 0)
		{
			/*
			 * We're using the right interface for this account
			 */
			if (accountIsAlreadyPending)
			{
				syslog(LOG_INFO, "We were already in the process of logging in");
				/* 
				 * keep the message in order to respond to it in either account_logged_in or account_login_failed 
				 */
				LSMessageRef(message);
				/*
				 * remove the old login message
				 */
				g_hash_table_remove(loginMessages, accountKey);
				/*
				 * add the new login message to respond to once account_logged_in is called
				 */
				g_hash_table_insert(loginMessages, accountKey, message);
				if (transportFriendlyUserName)
				{
					free(transportFriendlyUserName);
				}
				return TRUE;
			}
			else if (accountIsAlreadyOnline)
			{
				syslog(LOG_INFO, "We were already logged in to the requested account");
				json_object_object_add(responsePayload, "accountWasAlreadyLoggedIn", json_object_new_boolean(TRUE));
				json_object_object_add(responsePayload, "returnValue", json_object_new_boolean(TRUE));

				LSError lserror;
				LSErrorInit(&lserror);

				bool retVal = LSMessageReply(serviceHandle, message, json_object_to_json_string(responsePayload),
						&lserror);
				if (!retVal)
				{
					LSErrorPrint(&lserror, stderr);
				}
				if (transportFriendlyUserName)
				{
					free(transportFriendlyUserName);
				}
				return TRUE;
			}
		}
		else
		{
			/*
			 * We're not using the right interface. Close the current connection for this account and create a new one
			 */
			syslog(LOG_INFO,
					"We have to logout and login again since the local IP address has changed. Logging out from account");
			/*
			 * Once the current connection is closed we don't want to let java know that the account was disconnected. 
			 * Since java went down and came back up it didn't know that the account was connected anyways. 
			 * So let's take the account out of the account data hash and then disconnect it.
			 */
			if (g_hash_table_lookup(onlineAccountData, accountKey) != NULL)
			{
				g_hash_table_remove(onlineAccountData, accountKey);
			}
			if (g_hash_table_lookup(pendingAccountData, accountKey) != NULL)
			{
				g_hash_table_remove(pendingAccountData, accountKey);
			}
			purple_account_disconnect(alreadyActiveAccount);
		}
	}

	/*
	 * Let's go through our usual login process
	 */

	if (strcmp(username, "") == 0 || strcmp(password, "") == 0)
	{
		success = FALSE;
	}
	else
	{
		/* save the local IP address that we need to use */
		if (localIpAddress != NULL && strcmp(localIpAddress, "") != 0)
		{
			purple_prefs_remove("/purple/network/preferred_local_ip_address");
			purple_prefs_add_string("/purple/network/preferred_local_ip_address", localIpAddress);
		}
		else
		{
#ifdef DEVICE
			/*
			 * If we're on device you should not accept an empty ipAddress; it's mandatory to be provided
			 */
			success = FALSE;
			json_object_object_add(responsePayload, "errorCode", json_object_new_string("AcctMgr_Network_Error"));
			json_object_object_add(responsePayload, "errorText", json_object_new_string("localIpAddress was null or empty"));
			goto error;
#endif
		}

		/* save the local IP address that we need to use */
		if (connectionType != NULL && strcmp(connectionType, "") != 0)
		{
			g_hash_table_insert(connectionTypeData, accountKey, (char*)connectionType);
		}

		/*
		 * If we've already logged in to this account before then re-use the old PurpleAccount struct
		 */
		account = g_hash_table_lookup(offlineAccountData, accountKey);
		if (!account)
		{
			/* Create the account */
			account = purple_account_new(transportFriendlyUserName, prplProtocolId);
			if (!account)
			{
				success = FALSE;
				goto error;
			}
		}

		if (strcmp(prplProtocolId, "prpl-jabber") == 0 && g_str_has_suffix(transportFriendlyUserName, "@gmail.com")
				== FALSE)
		{
			/*
			 * Special case for gmail... don't try to connect to theraghavans.com if the username is nash@theraghavans.com
			 * Always connect to gmail.
			 */
			purple_account_set_string(account, "connect_server", "talk.google.com");
		}
		syslog(LOG_INFO, "Logging in...");

		free(transportFriendlyUserName);

		purple_account_set_password(account, password);

		if (registeredForAccountSignals == FALSE)
		{
			static int handle;
			/*
			 * Listen for a number of different signals:
			 */
			purple_signal_connect(purple_connections_get_handle(), "signed-on", &handle,
					PURPLE_CALLBACK(account_logged_in), NULL);
			purple_signal_connect(purple_connections_get_handle(), "signed-off", &handle,
					PURPLE_CALLBACK(account_signed_off_cb), NULL);

			purple_signal_connect(purple_connections_get_handle(), "account-status-changed", &handle,
					PURPLE_CALLBACK(account_status_changed), NULL);

			/*purple_signal_connect(purple_connections_get_handle(), "account-authorization-denied", &handle,
			 PURPLE_CALLBACK(account_login_failed), NULL);*/
			purple_signal_connect(purple_connections_get_handle(), "connection-error", &handle,
					PURPLE_CALLBACK(account_login_failed), NULL);
			registeredForAccountSignals = TRUE;
		}
	}

	if (success)
	{
		/* keep the message in order to respond to it in either account_logged_in or account_login_failed */
		LSMessageRef(message);
		g_hash_table_insert(loginMessages, accountKey, message);
		/* mark the account as pending */
		g_hash_table_insert(pendingAccountData, accountKey, account);

		if (localIpAddress != NULL && strcmp(localIpAddress, "") != 0)
		{
			/* keep track of the local IP address that we bound to when logging in to this account */
			g_hash_table_insert(ipAddressesBoundTo, accountKey, (char*)localIpAddress);
		}

		/* It's necessary to enable the account first. */
		purple_account_set_enabled(account, UI_ID, TRUE);

		void *blist_handle = purple_blist_get_handle();
		/* Now, to connect the account, create a status and activate it. */

		/*
		 * Create a timer for this account's login. If after 30 seconds login has not succ
		 */
		guint timerHandle = purple_timeout_add_seconds(CONNECT_TIMEOUT_SECONDS, connectTimeoutCallback, accountKey);
		g_hash_table_insert(accountLoginTimers, accountKey, (gpointer)timerHandle);

		PurpleStatusPrimitive prim = getPrplAvailabilityFromPalmAvailability(availability);
		PurpleSavedStatus *savedStatus = purple_savedstatus_new(NULL, prim);
		purple_savedstatus_set_message(savedStatus, customMessage);
		purple_savedstatus_activate_for_account(savedStatus, account);

		json_object_object_add(responsePayload, "returnValue", json_object_new_boolean(TRUE));
	}
	else
	{
		json_object_object_add(responsePayload, "errorCode", json_object_new_string("AcctMgr_Generic_Error"));
		json_object_object_add(responsePayload, "errorText", json_object_new_string("AcctMgr_Generic_Error"));
	}

	error:

	if (!success)
	{
		if (invalidParameters)
		{
			json_object_object_add(responsePayload, "returnValue", json_object_new_boolean(FALSE));
			json_object_object_add(responsePayload, "errorCode", json_object_new_string("1"));
			json_object_object_add(responsePayload, "errorText",
					json_object_new_string("Invalid parameter. Please double check the passed parameters."));
		}
		retVal = LSMessageReturn(lshandle, message, json_object_to_json_string(responsePayload), &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
	}
	//TODO: do I need to do this?
	// LSErrorFree (&lserror);
	if (myJavaFriendlyUsername)
	{
		free(myJavaFriendlyUsername);
	}
	if (prplProtocolId)
	{
		free(prplProtocolId);
	}
	if (!is_error(responsePayload)) 
	{
		json_object_put(responsePayload);
	}
	if (payload)
	{
		free(payload);
	}
	if (!is_error(params)) 
	{
		json_object_put(params);
	}
	return TRUE;
}

static bool logout(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	const char *serviceName = "";
	const char *username = "";

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	// get the message's payload (json object)
	json_t *object = LSMessageGetPayloadJSON(message);

	if (!object)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_string(object, "serviceName", &serviceName);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_string(object, "username", &username);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	syslog(LOG_INFO, "Parameters: servicename %s", serviceName);

	char *accountKey = getAccountKey(username, serviceName);

	PurpleAccount *accountTologoutFrom = g_hash_table_lookup(onlineAccountData, accountKey);
	if (accountTologoutFrom == NULL)
	{
		accountTologoutFrom = g_hash_table_lookup(pendingAccountData, accountKey);
		if (accountTologoutFrom == NULL)
		{
			GString *jsonResponse = g_string_new("{\"serviceName\":\"");
			g_string_append(jsonResponse, serviceName);
			g_string_append(jsonResponse, "\",  \"username\":\"");
			g_string_append(jsonResponse, username);
			g_string_append(
					jsonResponse,
					"\",  \"returnValue\":false, \"errorCode\":\"1\", \"errorText\":\"Trying to logout from an account that is not logged in\"}");
			bool retVal = LSMessageReturn(lshandle, message, jsonResponse->str, &lserror);
			if (!retVal)
			{
				LSErrorPrint(&lserror, stderr);
			}
			success = FALSE;
			LSErrorFree(&lserror);
			g_string_free(jsonResponse, TRUE);
			return TRUE;
		}
	}

	/* keep the message in order to respond to it in either account_logged_in or account_login_failed */
	LSMessageRef(message);
	g_hash_table_insert(logoutMessages, getAccountKeyFromPurpleAccount(accountTologoutFrom), message);

	purple_account_disconnect(accountTologoutFrom);

	error: if (!success)
	{
		retVal
				= LSMessageReturn(
						lshandle,
						message,
						"{\"returnValue\":false, \"errorCode\":\"1\", \"errorText\":\"Invalid parameter. Please double check the passed parameters.\"}",
						&lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		LSErrorFree(&lserror);
	}

	if (accountKey)
	{
		free(accountKey);
	}
	return TRUE;
}

static bool setMyAvailability(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	const char *serviceName = "";
	const char *username = "";
	int availability = 0;

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	// get the message's payload (json object)
	json_t *object = LSMessageGetPayloadJSON(message);

	if (!object)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_string(object, "serviceName", &serviceName);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_string(object, "username", &username);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_int(object, "availability", &availability);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	syslog(LOG_INFO, "Parameters: serviceName %s, availability %i", serviceName, availability);

	char *accountKey = getAccountKey(username, serviceName);

	PurpleAccount *account = g_hash_table_lookup(onlineAccountData, accountKey);

	if (account == NULL)
	{
		//this should never happen based on MessagingService's logic
		syslog(LOG_INFO,
				"setMyAvailability was called on an account that wasn't logged in. serviceName: %s, availability: %i",
				serviceName, availability);
		retVal = LSMessageReturn(lshandle, message, "{\"returnValue\":false}", &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		success = FALSE;
		goto error;
	}
	else
	{
		/*
		 * Let's get the current custom message and set it as well so that we don't overwrite it with ""
		 */
		PurplePresence *presence = purple_account_get_presence(account);
		const PurpleStatus *status = purple_presence_get_active_status(presence);
		const PurpleValue *value = purple_status_get_attr_value(status, "message");
		const char *customMessage = purple_value_get_string(value);
		if (customMessage == NULL)
		{
			customMessage = "";
		}

		PurpleStatusPrimitive prim = getPrplAvailabilityFromPalmAvailability(availability);
		PurpleStatusType *type = purple_account_get_status_type_with_primitive(account, prim);
		GList *attrs = NULL;
		attrs = g_list_append(attrs, "message");
		attrs = g_list_append(attrs, (char*)customMessage);
		purple_account_set_status_list(account, purple_status_type_get_id(type), TRUE, attrs);

		char availabilityString[2];
		sprintf(availabilityString, "%i", availability);

		GString *jsonResponse = g_string_new("{\"serviceName\":\"");
		g_string_append(jsonResponse, serviceName);
		g_string_append(jsonResponse, "\",  \"username\":\"");
		g_string_append(jsonResponse, username);
		g_string_append(jsonResponse, "\",  \"availability\":");
		g_string_append(jsonResponse, availabilityString);
		g_string_append(jsonResponse, ", \"returnValue\":true}");
		LSError lserror;
		LSErrorInit(&lserror);
		retVal = LSMessageReturn(lshandle, message, jsonResponse->str, &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		g_string_free(jsonResponse, TRUE);
	}

	error: if (!retVal)
	{
		syslog(LOG_INFO, "%s: sending response failed", __FUNCTION__);
	}
	LSErrorFree(&lserror);
	return TRUE;
}

static bool setMyCustomMessage(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	const char *serviceName = "";
	const char *username = "";
	const char *customMessage = "";

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	char *payload = strdup(LSMessageGetPayload(message));

	if (!payload)
	{
		success = FALSE;
		goto error;
	}

	struct json_object *params = json_tokener_parse(payload);
	if (is_error(params))
	{
		success = FALSE;
		goto error;
	}

	serviceName = getField(params, "serviceName");
	if (!serviceName)
	{
		success = FALSE;
		goto error;
	}

	username = getField(params, "username");
	if (!username)
	{
		success = FALSE;
		goto error;
	}
	customMessage = getField(params, "customMessage");
	if (!customMessage)
	{
		success = FALSE;
		goto error;
	}

	syslog(LOG_INFO, "Parameters: serviceName %s", serviceName);

	char *accountKey = getAccountKey(username, serviceName);

	PurpleAccount *account = g_hash_table_lookup(onlineAccountData, accountKey);
	if (account != NULL)
	{
		// get the account's current status type
		PurpleStatusType *type = purple_status_get_type(purple_account_get_active_status(account));
		GList *attrs = NULL;
		attrs = g_list_append(attrs, "message");
		attrs = g_list_append(attrs, (char*)customMessage);
		purple_account_set_status_list(account, purple_status_type_get_id(type), TRUE, attrs);

		struct json_object *payload = json_object_new_object();
		json_object_object_add(payload, "serviceName", json_object_new_string((char*)serviceName));
		json_object_object_add(payload, "username", json_object_new_string((char*)username));
		json_object_object_add(payload, "customMessage", json_object_new_string((char*)customMessage));
		json_object_object_add(payload, "returnValue", json_object_new_boolean(TRUE));
		LSError lserror;
		LSErrorInit(&lserror);

		retVal = LSMessageReturn(lshandle, message, json_object_to_json_string(payload), &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		if (!is_error(payload)) 
		{
			json_object_put(payload);
		}
	}

	error: if (!retVal)
	{
		syslog(LOG_INFO, "%s: sending response failed", __FUNCTION__);
	}
	LSErrorFree(&lserror);
	if (!is_error(params)) 
	{
		json_object_put(params);
	}
	return TRUE;
}

static bool getBuddyList(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	const char *serviceName = "";
	const char *username = "";
	bool subscribe = FALSE;

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	// get the message's payload (json object)
	json_t *object = LSMessageGetPayloadJSON(message);

	if (!object)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_string(object, "serviceName", &serviceName);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_string(object, "username", &username);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_bool(object, "subscribe", &subscribe);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	syslog(LOG_INFO, "Parameters: serviceName %s", serviceName);

	/* subscribe to the buddy list if subscribe:true is present. LSSubscriptionProcess takes care of this for us */
	bool subscribed;
	retVal = LSSubscriptionProcess(lshandle, message, &subscribed, &lserror);
	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}
	/*
	 * Send over the full buddy list if the account is already logged in
	 */
	char *accountKey = getAccountKey(username, serviceName);
	PurpleAccount *account = g_hash_table_lookup(onlineAccountData, accountKey);
	if (account != NULL)
	{
		respondWithFullBuddyList(account, (char*)serviceName, (char*)username);
	}

	error: LSErrorFree(&lserror);
	if (accountKey)
	{
		free(accountKey);
	}
	return TRUE;
}

static bool sendMessage(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	const char *serviceName = "";
	const char *username = "";
	const char *usernameTo = "";
	const char *messageText = "";

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	// get the message's payload (json object)
	char *payload = strdup(LSMessageGetPayload(message));

	if (!payload)
	{
		success = FALSE;
		goto error;
	}

	struct json_object *params = json_tokener_parse(payload);
	if (is_error(params))
	{
		success = FALSE;
		goto error;
	}

	serviceName = getField(params, "serviceName");
	if (!serviceName)
	{
		success = FALSE;
		goto error;
	}

	username = getField(params, "username");
	if (!username)
	{
		success = FALSE;
		goto error;
	}
	usernameTo = getField(params, "usernameTo");
	if (!usernameTo)
	{
		success = FALSE;
		goto error;
	}
	messageText = getField(params, "messageText");
	if (!messageText)
	{
		success = FALSE;
		goto error;
	}

	char *messageTextUnescaped = g_strcompress(messageText);

	char *accountKey = getAccountKey(username, serviceName);

	PurpleAccount *accountToSendFrom = g_hash_table_lookup(onlineAccountData, accountKey);
	if (accountToSendFrom == NULL)
	{
		retVal
				= LSMessageReturn(
						lshandle,
						message,
						"{\"returnValue\":false, \"errorCode\":\"11\", \"errorText\":\"Trying to send from an account that is not logged in\"}",
						&lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
		success = FALSE;
		goto error;
	}

	PurpleConversation *purpleConversation =
			purple_conversation_new(PURPLE_CONV_TYPE_IM, accountToSendFrom, usernameTo);
	purple_conv_im_send(purple_conversation_get_im_data(purpleConversation), messageTextUnescaped);

	retVal = LSMessageReturn(lshandle, message, "{\"returnValue\":true}", &lserror);
	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}

	error: LSErrorFree(&lserror);
	if (messageTextUnescaped)
	{
		free(messageTextUnescaped);
	}
	if (payload)
	{
		free(payload);
	}
	if (accountKey)
	{
		free(accountKey);
	}
	if (!is_error(params)) 
	{
		json_object_put(params);
	}
	return TRUE;
}

static bool registerForIncomingMessages(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool retVal;
	bool success = TRUE;
	LSError lserror;
	LSErrorInit(&lserror);

	/* Passed parameters */
	bool subscribe = FALSE;

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	// get the message's payload (json object)
	json_t *object = LSMessageGetPayloadJSON(message);

	if (!object)
	{
		success = FALSE;
		goto error;
	}

	retVal = json_get_bool(object, "subscribe", &subscribe);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	if (LSMessageIsSubscription(message))
	{
		bool subscribed;
		retVal = LSSubscriptionProcess(lshandle, message, &subscribed, &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
			retVal = LSMessageReply(lshandle, message,
					"{\"returnValue\": false, \"errorText\": \"Subscription error\"}", &lserror);
			if (!retVal)
			{
				LSErrorPrint(&lserror, stderr);
			}
			goto error;
		}

	}
	else
	{
		retVal = LSMessageReply(lshandle, message,
				"{\"returnValue\": false, \"errorText\": \"We were expecting a subscribe type message,"
					" but we did not receive one.\"}", &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
	}

	error: LSErrorFree(&lserror);
	return TRUE;
}







static bool enable(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	LSError lserror;
	LSErrorInit(&lserror);
	queuePresenceUpdates(TRUE);
	LSMessageReturn(lshandle, message, "{\"returnValue\":true}", &lserror);
	return TRUE;
}

static bool disable(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	LSError lserror;
	LSErrorInit(&lserror);
	//queuePresenceUpdates(FALSE);
	purple_timeout_add_seconds(DISABLE_QUEUE_TIMEOUT_SECONDS, queuePresenceUpdatesTimer, NULL);
	LSMessageReturn(lshandle, message, "{\"returnValue\":true}", &lserror);
	return TRUE;
}







static bool deviceConnectionClosed(LSHandle* lshandle, LSMessage *message, void *ctx)
{
	bool success = TRUE;
	LSError lserror;

	/* Passed parameters */
	const char *ipAddress = "";

	PurpleAccount *account;
	GString *jsonResponse;
	GSList *accountToLogoutList = NULL;
	GList *onlineAndPendingAccountKeys = NULL;
	GList *iterator = NULL;
	char *accountKey = "";
	GSList *accountIterator = NULL;

	syslog(LOG_INFO, "%s called.", __FUNCTION__);

	LSErrorInit(&lserror);

	// get the message's payload (json object)
	json_t *object = LSMessageGetPayloadJSON(message);

	if (!object)
	{
		success = FALSE;
		goto error;
	}

	bool retVal = json_get_string(object, "ipAddress", &ipAddress);
	if (!retVal)
	{
		success = FALSE;
		goto error;
	}

	syslog(LOG_INFO, "deviceConnectionClosed");

	onlineAndPendingAccountKeys = g_hash_table_get_keys(ipAddressesBoundTo);
	for (iterator = onlineAndPendingAccountKeys; iterator != NULL; iterator = g_list_next(iterator))
	{
		accountKey = iterator->data;
		char *accountBoundToIpAddress = g_hash_table_lookup(ipAddressesBoundTo, accountKey);
		if (accountBoundToIpAddress != NULL && strcmp(accountBoundToIpAddress, "") != 0 && strcmp(ipAddress,
				accountBoundToIpAddress) == 0)
		{
			boolean accountWasLoggedIn = FALSE;

			account = g_hash_table_lookup(onlineAccountData, accountKey);
			if (account == NULL)
			{
				account = g_hash_table_lookup(pendingAccountData, accountKey);
				if (account == NULL)
				{
					syslog(LOG_INFO, "account was not found in the hash");
					continue;
				}
				syslog(LOG_INFO, "Abandoning login");
			}
			else
			{
				accountWasLoggedIn = TRUE;
				syslog(LOG_INFO, "Logging out");
			}

			if (g_hash_table_lookup(onlineAccountData, accountKey) != NULL)
			{
				g_hash_table_remove(onlineAccountData, accountKey);
			}
			if (g_hash_table_lookup(pendingAccountData, accountKey) != NULL)
			{
				g_hash_table_remove(pendingAccountData, accountKey);
			}
			if (g_hash_table_lookup(offlineAccountData, accountKey) == NULL)
			{
				/*
				 * Keep the PurpleAccount struct to reuse in future logins
				 */
				g_hash_table_insert(offlineAccountData, accountKey, account);
			}
			
			purple_account_disconnect(account);

			accountToLogoutList = g_slist_append(accountToLogoutList, account);

			char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
			char *username = getJavaFriendlyUsername(account->username, serviceName);
			char *accountKey = getAccountKeyFromPurpleAccount(account);
			char *connectionType = g_hash_table_lookup(connectionTypeData, accountKey);
			if (connectionType == NULL)
			{
				connectionType = "";
			}

			GString *jsonResponse = g_string_new("{\"serviceName\":\"");
			g_string_append(jsonResponse, serviceName);
			g_string_append(jsonResponse, "\",  \"username\":\"");
			g_string_append(jsonResponse, username);

			if (accountWasLoggedIn)
			{
				g_string_append(
						jsonResponse,
						"\", \"returnValue\":false, \"errorCode\":\"AcctMgr_Network_Error\", \"errorText\":\"Connection failure\", \"connectionStatus\":\"loggedOut\", \"connectionType\":\"");
			}
			else
			{
				g_string_append(
						jsonResponse,
						"\", \"returnValue\":false, \"errorCode\":\"AcctMgr_Network_Error\", \"errorText\":\"Connection failure\", \"connectionType\":\"");
			}
			g_string_append(jsonResponse, connectionType);
			g_string_append(jsonResponse, "\"}");

			g_hash_table_remove(onlineAccountData, accountKey);
			// We can't remove this guy since we're iterating through its keys. We'll remove it after the break
			//g_hash_table_remove(ipAddressesBoundTo, accountKey);

			LSError lserror;
			LSErrorInit(&lserror);

			LSMessage *message = g_hash_table_lookup(loginMessages, accountKey);
			if (message != NULL)
			{
				retVal = LSMessageReply(serviceHandle, message, jsonResponse->str, &lserror);
				if (!retVal)
				{
					LSErrorPrint(&lserror, stderr);
				}
				g_hash_table_remove(loginMessages, accountKey);
				LSMessageUnref(message);
			}
			LSErrorFree(&lserror);
			free(serviceName);
			free(username);
			free(accountKey);
			g_string_free(jsonResponse, TRUE);
		}
	}

	if (g_slist_length(accountToLogoutList) == 0)
	{
		syslog(LOG_INFO, "No accounts were connected on the requested ip address");
		retVal = LSMessageReturn(lshandle, message, "{\"returnValue\":true}", &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
		}
	}
	else
	{
		for (accountIterator = accountToLogoutList; accountIterator != NULL; accountIterator = accountIterator->next)
		{
			account = (PurpleAccount *) accountIterator->data;
			char *serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
			char *username = getJavaFriendlyUsername(account->username, serviceName);
			char *accountKey = getAccountKeyFromPurpleAccount(account);
			g_hash_table_remove(ipAddressesBoundTo, accountKey);

			free(serviceName);
			free(username);
			free(accountKey);
		}
	}

	error: if (!success)
	{
		retVal = LSMessageReturn(lshandle, message, "{\"returnValue\":false}", &lserror);
	}
	else
	{
		retVal = LSMessageReturn(lshandle, message, "{\"returnValue\":true}", &lserror);
	}

	if (!retVal)
	{
		LSErrorPrint(&lserror, stderr);
	}

	LSErrorFree(&lserror);
	return TRUE;
}
/*
 * End of service methods
 */

/*
 * Methods exposed over the bus:
 */
static LSMethod methods[] =
{
{ "login", login },
{ "logout", logout },
{ "getBuddyList", getBuddyList },
{ "registerForIncomingMessages", registerForIncomingMessages },
{ "sendMessage", sendMessage },
{ "setMyAvailability", setMyAvailability },
{ "setMyCustomMessage", setMyCustomMessage },
{ "deviceConnectionClosed", deviceConnectionClosed },
{ "enable", enable },
{ "disable", disable },
{ }, 
};

int main(int argc, char *argv[])
{
	/* lunaservice variables */
	bool retVal = FALSE;
	LSError lserror;
	LSErrorInit(&lserror);

	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	if (loop == NULL)
		goto error;

	syslog(LOG_INFO, "Registering %s ... ", dbusAddress);
	g_message("Registering %s ... ", dbusAddress);

	retVal = LSRegister(dbusAddress, &serviceHandle, &lserror);
	if (!retVal)
		goto error;

	retVal = LSRegisterCategory(serviceHandle, "/", methods, NULL, NULL, &lserror);
	if (!retVal)
		goto error;

	syslog(LOG_INFO, "Succeeded.");
	g_message("Succeeded.");

	retVal = LSGmainAttach(serviceHandle, loop, &lserror);
	if (!retVal)
		goto error;

	//TODO: replace the NULLs with real functions to prevent memory leaks
	onlineAccountData = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	pendingAccountData = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	accountLoginTimers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	loginMessages = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	logoutMessages = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	ipAddressesBoundTo = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free);
	connectionTypeData = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free);
	offlineAccountData = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

	g_main_loop_run(loop); 

	error: if (LSErrorIsSet(&lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	if (serviceHandle)
	{
		retVal = LSUnregister(serviceHandle, &lserror);
		if (!retVal)
		{
			LSErrorPrint(&lserror, stderr);
			LSErrorFree(&lserror);
		}
	}

	if (loop)
		g_main_loop_unref(loop);

	return 0;
}
