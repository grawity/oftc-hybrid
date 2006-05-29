/*
 *  ircd-hybrid: an advanced Internet Relay Chat Daemon(ircd).
 *
 *  Copyright (C) 2002 by the past and present ircd coders, and others.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

/*! \file channel.c
 * \brief Responsible for managing channels, members, bans and topics
 * \version $Id$
 */

#include "stdinc.h"
#include "channel.h"
#include "channel_mode.h"
#include "client.h"
#include "common.h"
#include "hash.h"
#include "ircd.h"
#include "numeric.h"
#include "s_serv.h"             /* captab */
#include "s_user.h"
#include "send.h"
#include "s_conf.h"             /* ConfigFileEntry, ConfigChannel */

struct config_channel_entry ConfigChannel;
dlink_list global_channel_list = { NULL, NULL, 0 };
dlink_list lazylink_channels = { NULL, NULL, 0 };
BlockHeap *ban_heap;    /*! \todo ban_heap shouldn't be a global var */

static BlockHeap *topic_heap = NULL;
static BlockHeap *member_heap = NULL;
static BlockHeap *channel_heap = NULL;

static char buf[IRCD_BUFSIZE];
static char modebuf[MODEBUFLEN];
static char parabuf[MODEBUFLEN];


/*! \brief Initializes the channel blockheap, adds known channel CAPAB
 */
void
init_channels(void)
{
  /*
   * XXX - These should get moved to somwhere else once we have 
   * a modular channelmode system
   */
  add_capability("EX", CAP_EX, 1);
  add_capability("IE", CAP_IE, 1);
  add_capability("CHW", CAP_CHW, 1);

  channel_heap = BlockHeapCreate("channel", sizeof(struct Channel), CHANNEL_HEAP_SIZE);
  ban_heap = BlockHeapCreate("ban", sizeof(struct Ban), BAN_HEAP_SIZE);
  topic_heap = BlockHeapCreate("topic", TOPICLEN+1 + USERHOST_REPLYLEN, TOPIC_HEAP_SIZE);
  member_heap = BlockHeapCreate("member", sizeof(struct Membership), CHANNEL_HEAP_SIZE*2);
}

/*! \brief adds a user to a channel by adding another link to the
 *         channels member chain.
 * \param chptr      pointer to channel to add client to
 * \param who        pointer to client (who) to add
 * \param flags      flags for chanops etc
 * \param flood_ctrl whether to count this join in flood calculations
 */
void
add_user_to_channel(struct Channel *chptr, struct Client *who,
                    unsigned int flags, int flood_ctrl)
{
  struct Membership *ms = NULL;

  if (GlobalSetOptions.joinfloodtime > 0)
  {
    if (flood_ctrl)
      chptr->number_joined++;

    chptr->number_joined -= (CurrentTime - chptr->last_join_time) *
      (((float)GlobalSetOptions.joinfloodcount) /
       (float)GlobalSetOptions.joinfloodtime);

    if (chptr->number_joined <= 0)
    {
      chptr->number_joined = 0;
      ClearJoinFloodNoticed(chptr);
    }
    else if (chptr->number_joined >= GlobalSetOptions.joinfloodcount)
    {
      chptr->number_joined = GlobalSetOptions.joinfloodcount;

      if (!IsSetJoinFloodNoticed(chptr))
      {
        SetJoinFloodNoticed(chptr);
        sendto_gnotices_flags(UMODE_BOTS, L_ALL, me.name, &me, NULL,
                             "Possible Join Flooder %s on %s target: %s",
                             get_client_name(who, HIDE_IP),
                             who->servptr->name, chptr->chname);
      }
    }

    chptr->last_join_time = CurrentTime;
  }

  ms = BlockHeapAlloc(member_heap);
  ms->client_p = who;
  ms->chptr = chptr;
  ms->flags = flags;

  dlinkAdd(ms, &ms->channode, &chptr->members);

  if (MyConnect(who))
    dlinkAdd(ms, &ms->locchannode, &chptr->locmembers);

  dlinkAdd(ms, &ms->usernode, &who->channel);
}

/*! \brief deletes an user from a channel by removing a link in the
 *         channels member chain.
 * \param member pointer to Membership struct
 */
void
remove_user_from_channel(struct Membership *member)
{
  struct Client *client_p = member->client_p;
  struct Channel *chptr = member->chptr;

  dlinkDelete(&member->channode, &chptr->members);

  if (MyConnect(client_p))
    dlinkDelete(&member->locchannode, &chptr->locmembers);

  dlinkDelete(&member->usernode, &client_p->channel);

  BlockHeapFree(member_heap, member);

  if (dlink_list_length(&chptr->members) == 0)
  {
    assert(dlink_list_length(&chptr->locmembers) == 0);
    destroy_channel(chptr);
  }
}

/* send_members()
 *
 * inputs       -
 * output       - NONE
 * side effects -
 */
static void
send_members(struct Client *client_p, struct Channel *chptr,
             char *lmodebuf, char *lparabuf)
{
  struct Membership *ms;
  dlink_node *ptr;
  int tlen;              /* length of text to append */
  char *t, *start;       /* temp char pointer */

  start = t = buf + ircsprintf(buf, ":%s SJOIN %lu %s %s %s:",
                               ID_or_name(&me, client_p),
                               (unsigned long)chptr->channelts,
                               chptr->chname, lmodebuf, lparabuf);

  DLINK_FOREACH(ptr, chptr->members.head)
  {
    ms = ptr->data;

    tlen = strlen(IsCapable(client_p, CAP_TS6) ?
      ID(ms->client_p) : ms->client_p->name) + 1;  /* nick + space */

    if (ms->flags & CHFL_CHANOP)
      tlen++;
#ifdef HALFOPS
    if (ms->flags & CHFL_HALFOP)
      tlen++;
#endif
    if (ms->flags & CHFL_VOICE)
      tlen++;

    /* space will be converted into CR, but we also need space for LF..
     * That's why we use '- 1' here
     * -adx */
    if (t + tlen - buf > sizeof(buf) - 1)
    {
      *(t - 1) = '\0';  /* kill the space and terminate the string */
      sendto_one(client_p, "%s", buf);
      t = start;
    }

    strcpy(t, get_member_status(ms, YES));
    t += strlen(t);

    if (IsCapable(client_p, CAP_TS6))
      strcpy(t, ID(ms->client_p));
    else
      strcpy(t, ms->client_p->name);
    t += strlen(t);
    *t++ = ' ';
  }

  /* should always be non-NULL unless we have a kind of persistent channels */
  if (chptr->members.head != NULL)
    t--;  /* take the space out */
  *t = '\0';
  sendto_one(client_p, "%s", buf);
}

/*! \brief sends +b/+e/+I
 * \param client_p client pointer to server
 * \param chptr    pointer to channel
 * \param top      pointer to top of mode link list to send
 * \param flag     char flag flagging type of mode. Currently this can be 'b', e' or 'I'
 */
static void
send_mode_list(struct Client *client_p, struct Channel *chptr,
               dlink_list *top, char flag)
{
  int ts5 = !IsCapable(client_p, CAP_TS6);
  dlink_node *lp;
  struct Ban *banptr;
  char pbuf[IRCD_BUFSIZE];
  int tlen, mlen, cur_len, count = 0;
  char *mp = NULL, *pp = pbuf;

  if (top == NULL || top->length == 0)
    return;

  if (ts5)
    mlen = ircsprintf(buf, ":%s MODE %s +", me.name, chptr->chname);
  else
    mlen = ircsprintf(buf, ":%s BMASK %lu %s %c :", me.id,
                      (unsigned long)chptr->channelts, chptr->chname, flag);

  /* MODE needs additional one byte for space between buf and pbuf */
  cur_len = mlen + ts5;
  mp = buf + mlen;

  DLINK_FOREACH(lp, top->head)
  {
    banptr = lp->data;

    /* must add another b/e/I letter if we use MODE */
    tlen = banptr->len + 3 + ts5;

    /*
     * send buffer and start over if we cannot fit another ban,
     * or if the target is non-ts6 and we have too many modes in
     * in this line.
     */
    if (cur_len + (tlen - 1) > IRCD_BUFSIZE - 2 ||
        (!IsCapable(client_p, CAP_TS6) &&
         (count >= MAXMODEPARAMS || pp - pbuf >= MODEBUFLEN)))
    {
      *(pp - 1) = '\0';  /* get rid of trailing space on buffer */
      sendto_one(client_p, "%s%s%s", buf, ts5 ? " " : "", pbuf);

      cur_len = mlen + ts5;
      mp = buf + mlen;
      pp = pbuf;
      count = 0;
    }

    count++;
    if (ts5)
    {
      *mp++ = flag;
      *mp = '\0';
    }

    pp += ircsprintf(pp, "%s!%s@%s ", banptr->name, banptr->username,
                     banptr->host);
    cur_len += tlen;
  }

  *(pp - 1) = '\0';  /* get rid of trailing space on buffer */
  sendto_one(client_p, "%s%s%s", buf, ts5 ? " " : "", pbuf);
}

/*! \brief send "client_p" a full list of the modes for channel chptr
 * \param client_p pointer to client client_p
 * \param chptr    pointer to channel pointer
 */
void
send_channel_modes(struct Client *client_p, struct Channel *chptr)
{
  if (chptr->chname[0] != '#')
    return;

  *modebuf = *parabuf = '\0';
  channel_modes(chptr, client_p, modebuf, parabuf);
  send_members(client_p, chptr, modebuf, parabuf);

  send_mode_list(client_p, chptr, &chptr->banlist, 'b');

  if (IsCapable(client_p, CAP_EX))
    send_mode_list(client_p, chptr, &chptr->exceptlist, 'e');
  if (IsCapable(client_p, CAP_IE))
    send_mode_list(client_p, chptr, &chptr->invexlist, 'I');
}

/*! \brief check channel name for invalid characters
 * \param name pointer to channel name string
 * \return TRUE (1) if name ok, FALSE (0) otherwise
 */
int
check_channel_name(const char *name)
{
  const unsigned char *p = (const unsigned char *)name;
  assert(name != NULL);

  for (; *p; ++p)
    if (!IsChanChar(*p))
      return 0;

  return 1;
}

void
remove_ban(struct Ban *bptr, dlink_list *list)
{
  dlinkDelete(&bptr->node, list);

  MyFree(bptr->name);
  MyFree(bptr->username);
  MyFree(bptr->host);
  MyFree(bptr->who);

  BlockHeapFree(ban_heap, bptr);
}

/* free_channel_list()
 *
 * inputs       - pointer to dlink_list
 * output       - NONE
 * side effects -
 */
void
free_channel_list(dlink_list *list)
{
  dlink_node *ptr = NULL, *next_ptr = NULL;

  DLINK_FOREACH_SAFE(ptr, next_ptr, list->head)
    remove_ban(ptr->data, list);

  assert(list->tail == NULL && list->head == NULL);
}

/*! \brief Get Channel block for chname (and allocate a new channel
 *         block, if it didn't exist before)
 * \param client_p client pointer
 * \param chname   channel name
 * \param isnew    pointer to int flag whether channel was newly created or not
 * \return channel block or NULL if illegal name
 */
struct Channel *
get_or_create_channel(struct Client *client_p, const char *chname, int *isnew)
{
  struct Channel *chptr = NULL;
  int len;

  if (EmptyString(chname))
    return NULL;

  if ((len = strlen(chname)) > CHANNELLEN)
  {
    if (IsServer(client_p))
      sendto_gnotice_flags(UMODE_DEBUG, L_ALL, me.name, &me, NULL,
                           "*** Long channel name from %s (%d > %d): %s",
                           client_p->name, len, CHANNELLEN, chname);
    return NULL;
  }

  if ((chptr = hash_find_channel(chname)) != NULL)
  {
    if (isnew != NULL)
      *isnew = 0;

    return chptr;
  }

  if (isnew != NULL)
    *isnew = 1;

  chptr = BlockHeapAlloc(channel_heap);
  /* doesn't hurt to set it here */
  chptr->channelts = chptr->last_join_time = CurrentTime;

  strlcpy(chptr->chname, chname, sizeof(chptr->chname));
  dlinkAdd(chptr, &chptr->node, &global_channel_list);

  hash_add_channel(chptr);

  return chptr;
}

/*! \brief walk through this channel, and destroy it.
 * \param chptr channel pointer
 */
void
destroy_channel(struct Channel *chptr)
{
  dlink_node *ptr = NULL, *ptr_next = NULL;

  DLINK_FOREACH_SAFE(ptr, ptr_next, chptr->invites.head)
    del_invite(chptr, ptr->data);

  /* free ban/exception/invex lists */
  free_channel_list(&chptr->banlist);
  free_channel_list(&chptr->exceptlist);
  free_channel_list(&chptr->invexlist);

  /* Free the topic */
  free_topic(chptr);

  dlinkDelete(&chptr->node, &global_channel_list);
  hash_del_channel(chptr);

  if (ServerInfo.hub)
    if ((ptr = dlinkFindDelete(&lazylink_channels, chptr)))
      free_dlink_node(ptr);

  BlockHeapFree(channel_heap, chptr);
}

/*!
 * \param chptr pointer to channel
 * \return string pointer "=" if public, "@" if secret else "*"
 */
static const char *
channel_pub_or_secret(struct Channel *chptr)
{
  if (SecretChannel(chptr))
    return "@";
  if (PrivateChannel(chptr))
    return "*";
  return "=";
}

/*! \brief lists all names on given channel
 * \param source_p pointer to client struct requesting names
 * \param chptr    pointer to channel block
 * \param show_eon show ENDOFNAMES numeric or not
 *                 (don't want it with /names with no params)
 */
void
channel_member_names(struct Client *source_p, struct Channel *chptr,
                     int show_eon)
{
  struct Client *target_p = NULL;
  struct Membership *ms = NULL;
  dlink_node *ptr = NULL;
  char lbuf[IRCD_BUFSIZE + 1];
  char *t = NULL, *start = NULL;
  int tlen = 0;
  int is_member = IsMember(source_p, chptr);

  if (PubChannel(chptr) || is_member || IsGod(source_p))
  {
    t = lbuf + ircsprintf(lbuf, form_str(RPL_NAMREPLY),
                          me.name, source_p->name,
                          channel_pub_or_secret(chptr),
                          chptr->chname);
    start = t;

    DLINK_FOREACH(ptr, chptr->members.head)
    {
      ms       = ptr->data;
      target_p = ms->client_p;

      if (IsInvisible(target_p) && !is_member && !IsGod(source_p))
        continue;

      tlen = strlen(target_p->name) + 1;  /* nick + space */

      if (ms->flags & (CHFL_CHANOP | CHFL_HALFOP | CHFL_VOICE))
        ++tlen;
      if (t + tlen - lbuf > IRCD_BUFSIZE)
      {
        *(t - 1) = '\0';
        sendto_one(source_p, "%s", lbuf);
        t = start;
      }

      t += ircsprintf(t, "%s%s ", get_member_status(ms, NO),
                      target_p->name);
    }

    if (tlen != 0)
    {
      *(t - 1) = '\0';
      sendto_one(source_p, "%s", lbuf);
    }
  }

  if (show_eon)
    sendto_one(source_p, form_str(RPL_ENDOFNAMES),
               me.name, source_p->name, chptr->chname);
}

/*! \brief adds client to invite list
 * \param chptr pointer to channel block
 * \param who   pointer to client to add invite to
 */
void
add_invite(struct Channel *chptr, struct Client *who)
{
  del_invite(chptr, who);

  /*
   * delete last link in chain if the list is max length
   */
  if (dlink_list_length(&who->invited) >=
      ConfigChannel.max_chans_per_user)
    del_invite(who->invited.tail->data, who);

  /* add client to channel invite list */
  dlinkAdd(who, make_dlink_node(), &chptr->invites);

  /* add channel to the end of the client invite list */
  dlinkAdd(chptr, make_dlink_node(), &who->invited);
}

/*! \brief Delete Invite block from channel invite list
 *         and client invite list
 * \param chptr pointer to Channel struct
 * \param who   pointer to client to remove invites from
 */
void
del_invite(struct Channel *chptr, struct Client *who)
{
  dlink_node *ptr = NULL;

  if ((ptr = dlinkFindDelete(&who->invited, chptr)))
    free_dlink_node(ptr);

  if ((ptr = dlinkFindDelete(&chptr->invites, who)))
    free_dlink_node(ptr);
}

/* get_member_status()
 *
 * inputs       - pointer to struct Membership
 *              - YES if we can combine different flags
 * output       - string either @, +, % or "" depending on whether
 *                chanop, voiced or user
 * side effects -
 *
 * NOTE: Returned string is usually a static buffer
 * (like in get_client_name)
 */
const char *
get_member_status(const struct Membership *ms, int combine)
{
  static char buffer[4];
  char *p = NULL;

  if (ms == NULL)
    return "";
  p = buffer;

  if (ms->flags & CHFL_CHANOP)
  {
    if (!combine)
      return "@";
    *p++ = '@';
  }

#ifdef HALFOPS
  if (ms->flags & CHFL_HALFOP)
  {
    if (!combine)
      return "%";
    *p++ = '%';
  }
#endif

  if (ms->flags & CHFL_VOICE)
    *p++ = '+';
  *p = '\0';

  return buffer;
}

/*!
 * \param who  pointer to Client to check
 * \param list pointer to ban list to search
 * \return 1 if ban found for given n!u\@h mask, 0 otherwise
 *
 */
static int
find_bmask(const struct Client *who, const dlink_list *const list)
{
  const dlink_node *ptr = NULL;

  DLINK_FOREACH(ptr, list->head)
  {
    const struct Ban *bp = ptr->data;

    if (match(bp->name, who->name) &&
        match(bp->username, who->username) &&
        (match(bp->host, who->host) ||
         match(bp->host, who->sockhost) ||
         match_cidr(bp->host, who->sockhost)))
      return 1;
  }

  return 0;
}

/*!
 * \param chptr pointer to channel block
 * \param who   pointer to client to check access fo
 * \return 0 if not banned, 1 otherwise
 */
int
is_banned(struct Channel *chptr, struct Client *who)
{
  assert(IsClient(who));

  return find_bmask(who, &chptr->banlist) && (!ConfigChannel.use_except ||
         !find_bmask(who, &chptr->exceptlist));
}

/*!
 * \param source_p pointer to client attempting to join
 * \param chptr    pointer to channel 
 * \param key      key sent by client attempting to join if present
 * \return ERR_BANNEDFROMCHAN, ERR_INVITEONLYCHAN, ERR_CHANNELISFULL
 *         or 0 if allowed to join.
 */
int
can_join(struct Client *source_p, struct Channel *chptr, const char *key)
{
  if (find_bmask(source_p, &chptr->banlist))
    if (!ConfigChannel.use_except || !find_bmask(source_p, &chptr->exceptlist))
      return ERR_BANNEDFROMCHAN;

  if (chptr->mode.mode & MODE_INVITEONLY)
    if (!dlinkFind(&source_p->invited, chptr))
      if (!ConfigChannel.use_invex || !find_bmask(source_p, &chptr->invexlist))
        return ERR_INVITEONLYCHAN;

  if (chptr->mode.key[0] && (EmptyString(key) || irccmp(chptr->mode.key, key)))
    return ERR_BADCHANNELKEY;

  if (chptr->mode.limit && dlink_list_length(&chptr->members) >=
      chptr->mode.limit)
    return ERR_CHANNELISFULL;

  return 0;
}

int
has_member_flags(struct Membership *ms, unsigned int flags)
{
  if (ms != NULL)
    return ms->flags & flags;
  return 0;
}

struct Membership *
find_channel_link(struct Client *client_p, struct Channel *chptr)
{
  dlink_node *ptr = NULL;

  if (!IsClient(client_p))
    return NULL;

  DLINK_FOREACH(ptr, client_p->channel.head)
    if (((struct Membership *)ptr->data)->chptr == chptr)
      return (struct Membership *)ptr->data;

  return NULL;
}

/*!
 * \param chptr    pointer to Channel struct
 * \param source_p pointer to Client struct
 * \return CAN_SEND_OPV if op or voiced on channel\n
 *         CAN_SEND_NONOP if can send to channel but is not an op\n
 *         CAN_SEND_NO if they cannot send to channel\n
 */
int
can_send(struct Channel *chptr, struct Client *source_p)
{
  struct Membership *ms = NULL;

  if (IsServer(source_p))
    return CAN_SEND_OPV;

  if (MyClient(source_p) && !IsExemptResv(source_p) &&
      !(IsOper(source_p) && ConfigFileEntry.oper_pass_resv) &&
      (!hash_find_resv(chptr->chname) == ConfigChannel.restrict_channels))
    return CAN_SEND_NO;

  if ((ms = find_channel_link(source_p, chptr)) == NULL)
  {
    if (chptr->mode.mode & MODE_NOPRIVMSGS)
      return CAN_SEND_NO;
  }
  else
  {
    if (ms->flags & (CHFL_CHANOP|CHFL_HALFOP|CHFL_VOICE))
      return CAN_SEND_OPV;

    /* cache can send if quiet_on_ban and banned */
    if (ConfigChannel.quiet_on_ban && MyClient(source_p))
    {
      if (ms->flags & CHFL_BAN_SILENCED)
        return CAN_SEND_NO;

      if (!(ms->flags & CHFL_BAN_CHECKED))
      {
        if (is_banned(chptr, source_p))
        {
          ms->flags |= (CHFL_BAN_CHECKED|CHFL_BAN_SILENCED);
          return CAN_SEND_NO;
        }

        ms->flags |= CHFL_BAN_CHECKED;
      }
    }
  }

  if (chptr->mode.mode & MODE_MODERATED)
    return CAN_SEND_NO;

  return CAN_SEND_NONOP;
}

/*! \brief Checks to see if given client can send a part message
 * \param member     pointer to channel membership
 * \param chptr      pointer to channel struct
 * \param source_p   pointer to struct Client to check
 */
int
can_send_part(struct Membership *member, struct Channel *chptr,
              struct Client *source_p)
{
  if (has_member_flags(member, CHFL_CHANOP|CHFL_HALFOP))
    return CAN_SEND_OPV;

  if (chptr->mode.mode & MODE_MODERATED)
    return CAN_SEND_NO;

  if (ConfigChannel.quiet_on_ban && MyClient(source_p) &&
      is_banned(chptr, source_p))
    return CAN_SEND_NO;

  return CAN_SEND_NONOP;
}

/*! \brief Updates the client's oper_warn_count_down, warns the
 *         IRC operators if necessary, and updates
 *         join_leave_countdown as needed.
 * \param source_p pointer to struct Client to check
 * \param name     channel name or NULL if this is a part.
 */
void
check_spambot_warning(struct Client *source_p, const char *name)
{
  int t_delta = 0;
  int decrement_count = 0;

  if ((GlobalSetOptions.spam_num &&
       (source_p->localClient->join_leave_count >=
        GlobalSetOptions.spam_num)))
  {
    if (source_p->localClient->oper_warn_count_down > 0)
      source_p->localClient->oper_warn_count_down--;
    else
      source_p->localClient->oper_warn_count_down = 0;

    if (source_p->localClient->oper_warn_count_down == 0)
    {
      /* Its already known as a possible spambot */
      if (name != NULL)
        sendto_gnotice_flags(UMODE_BOTS, L_ALL, me.name, &me, NULL,
                             "User %s (%s@%s) trying to join %s is a possible spambot",
                             source_p->name, source_p->username,
                             source_p->host, name);
      else
        sendto_gnotice_flags(UMODE_BOTS, L_ALL, me.name, &me, NULL,
                             "User %s (%s@%s) is a possible spambot",
                             source_p->name, source_p->username,
                             source_p->host);
      source_p->localClient->oper_warn_count_down = OPER_SPAM_COUNTDOWN;
    }
  }
  else
  {
    if ((t_delta = (CurrentTime - source_p->localClient->last_leave_time)) >
         JOIN_LEAVE_COUNT_EXPIRE_TIME)
    {
      decrement_count = (t_delta / JOIN_LEAVE_COUNT_EXPIRE_TIME);
      if (decrement_count > source_p->localClient->join_leave_count)
        source_p->localClient->join_leave_count = 0;
      else
        source_p->localClient->join_leave_count -= decrement_count;
    }
    else
    {
      if ((CurrentTime - (source_p->localClient->last_join_time)) <
          GlobalSetOptions.spam_time)
      {
        /* oh, its a possible spambot */
        source_p->localClient->join_leave_count++;
      }
    }

    if (name != NULL)
      source_p->localClient->last_join_time = CurrentTime;
    else
      source_p->localClient->last_leave_time = CurrentTime;
  }
}

/*! \brief compares usercount and servercount against their split
 *         values and adjusts splitmode accordingly
 * \param unused Unused address pointer
 */
void
check_splitmode(void *unused)
{
  if (splitchecking && (ConfigChannel.no_join_on_split ||
                        ConfigChannel.no_create_on_split))
  {
    const unsigned int server = dlink_list_length(&global_serv_list);

    if (!splitmode && ((server < split_servers) || (Count.total < split_users)))
    {
      splitmode = 1;

      sendto_gnotice_flags(UMODE_ALL,L_ALL, me.name, &me, NULL,
                           "Network split, activating splitmode");
      eventAddIsh("check_splitmode", check_splitmode, NULL, 10);
    }
    else if (splitmode && (server > split_servers) && (Count.total > split_users))
    {
      splitmode = 0;

      sendto_gnotice_flags(UMODE_ALL, L_ALL, me.name, &me, NULL,
                           "Network rejoined, deactivating splitmode");
      eventDelete(check_splitmode, NULL);
    }
  }
}

/*! \brief Allocates a new topic
 * \param chptr Channel to allocate a new topic for
 */
static void
allocate_topic(struct Channel *chptr)
{
  void *ptr = NULL;

  if (chptr == NULL)
    return;

  ptr = BlockHeapAlloc(topic_heap);  

  /* Basically we allocate one large block for the topic and
   * the topic info.  We then split it up into two and shove it
   * in the chptr 
   */
  chptr->topic       = ptr;
  chptr->topic_info  = (char *)ptr + TOPICLEN+1;
  *chptr->topic      = '\0';
  *chptr->topic_info = '\0';
}

void
free_topic(struct Channel *chptr)
{
  void *ptr = NULL;
  assert(chptr);
  if (chptr->topic == NULL)
    return;

  /*
   * If you change allocate_topic you MUST change this as well
   */
  ptr = chptr->topic; 
  BlockHeapFree(topic_heap, ptr);    
  chptr->topic      = NULL;
  chptr->topic_info = NULL;
}

/*! \brief Sets the channel topic for chptr
 * \param chptr      Pointer to struct Channel
 * \param topic      The topic string
 * \param topic_info n!u\@h formatted string of the topic setter
 * \param topicts    timestamp on the topic
 */
void
set_channel_topic(struct Channel *chptr, const char *topic,
                  const char *topic_info, time_t topicts)
{
  if (!EmptyString(topic))
  {
    if (chptr->topic == NULL)
      allocate_topic(chptr);

    strlcpy(chptr->topic, topic, TOPICLEN+1);
    strlcpy(chptr->topic_info, topic_info, USERHOST_REPLYLEN);
    chptr->topic_time = topicts; 
  }
  else
  {
    if (chptr->topic != NULL)
      free_topic(chptr);

    chptr->topic_time = 0;
  }
}

int msg_has_colors(char *msg)
{

    char *c;
    if (msg == NULL) 
        return 0;
    c = msg;
    
    while(*c) 
    {   
        if(*c == '\003' || *c == '\033')
            break;
    else       
        c++;
    }   

    if(*c)
        return 1;
    return 0;
}

char *strip_color(char* string)
{
    char *source = string;
    char *dest = string;
    char *last_non_space = NULL;

    for (; source && *source; source++)
        switch(*source)
        {
        case 3:
            if (isdigit(source[1]))
            {
                source++;
                if (isdigit(source[1]))
                    source++;
                if (source[1] == ',' && isdigit(source[2]))
                {
                    source+=2;
                    if (isdigit(source[1]))
                        source++;
                }
            }
            break;
        case 2: case 6: case 7: case 22: case 23: case 27: case 31:
            break;
        case 32:
            *dest++ = *source;
            break;
        default:
            *dest++ = *source;
            last_non_space = dest;
            break;
        }
    *dest = '\0';
    if (last_non_space)
        *last_non_space = '\0';
    return string;
}
