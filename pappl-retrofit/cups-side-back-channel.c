/*
 * Side-channel API code for CUPS.
 *
 * Copyright © 2007-2019 by Apple Inc.
 * Copyright © 2006 by Easy Software Products.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

/*
 * Include necessary headers...
 */

#include <pappl-retrofit/cups-side-back-channel-private.h>
#include <cups/cups.h>
#include <errno.h>
#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <stdlib.h>
#  include <string.h>
#endif /* _WIN32 */
#ifdef HAVE_POLL
#  include <poll.h>
#endif /* HAVE_POLL */


/*
 * Buffer size for side-channel requests...
 */

#define _CUPS_SC_MAX_DATA	65535
#define _CUPS_SC_MAX_BUFFER	65540


/*
 * Local functions...
 */

/*
 * 'cups_setup()' - Setup select()
 */

static void
cups_setup(fd_set         *set,		/* I - Set for select() */
           struct timeval *tval,	/* I - Timer value */
	   double         timeout)	/* I - Timeout in seconds */
{
  tval->tv_sec = (time_t)timeout;
  tval->tv_usec = (suseconds_t)(1000000.0 * (timeout - tval->tv_sec));

  FD_ZERO(set);
  FD_SET(3, set);
}


/*
 * '_prBackChannelRead()' - Read data from the backchannel.
 *
 * Reads up to "bytes" bytes from the backchannel/backend. The "timeout"
 * parameter controls how many seconds to wait for the data - use 0.0 to
 * return immediately if there is no data, -1.0 to wait for data indefinitely.
 *
 * @since CUPS 1.2/macOS 10.5@
 */

ssize_t					/* O - Bytes read or -1 on error */
_prBackChannelRead(char   *buffer,	/* I - Buffer to read into */
                    size_t bytes,	/* I - Bytes to read */
		    double timeout)	/* I - Timeout in seconds, typically 0.0 to poll */
{
  fd_set	input;			/* Input set */
  struct timeval tval;			/* Timeout value */
  int		status;			/* Select status */


 /*
  * Wait for input ready.
  */

  do
  {
    cups_setup(&input, &tval, timeout);

    if (timeout < 0.0)
      status = select(4, &input, NULL, NULL, NULL);
    else
      status = select(4, &input, NULL, NULL, &tval);
  }
  while (status < 0 && errno != EINTR && errno != EAGAIN);

  if (status <= 0)
    return (-1);			/* Timeout! */

 /*
  * Read bytes from the pipe...
  */

#ifdef _WIN32
  return ((ssize_t)_read(3, buffer, (unsigned)bytes));
#else
  return (read(3, buffer, bytes));
#endif /* _WIN32 */
}


/*
 * '_prBackChannelWrite()' - Write data to the backchannel.
 *
 * Writes "bytes" bytes to the backchannel/filter. The "timeout" parameter
 * controls how many seconds to wait for the data to be written - use
 * 0.0 to return immediately if the data cannot be written, -1.0 to wait
 * indefinitely.
 *
 * @since CUPS 1.2/macOS 10.5@
 */

ssize_t					/* O - Bytes written or -1 on error */
_prBackChannelWrite(
    const char *buffer,			/* I - Buffer to write */
    size_t     bytes,			/* I - Bytes to write */
    double     timeout)			/* I - Timeout in seconds, typically 1.0 */
{
  fd_set	output;			/* Output set */
  struct timeval tval;			/* Timeout value */
  int		status;			/* Select status */
  ssize_t	count;			/* Current bytes */
  size_t	total;			/* Total bytes */


 /*
  * Write all bytes...
  */

  total = 0;

  while (total < bytes)
  {
   /*
    * Wait for write-ready...
    */

    do
    {
      cups_setup(&output, &tval, timeout);

      if (timeout < 0.0)
	status = select(4, NULL, &output, NULL, NULL);
      else
	status = select(4, NULL, &output, NULL, &tval);
    }
    while (status < 0 && errno != EINTR && errno != EAGAIN);

    if (status <= 0)
      return (-1);			/* Timeout! */

   /*
    * Write bytes to the pipe...
    */

#ifdef _WIN32
    count = (ssize_t)_write(3, buffer, (unsigned)(bytes - total));
#else
    count = write(3, buffer, bytes - total);
#endif /* _WIN32 */

    if (count < 0)
    {
     /*
      * Write error - abort on fatal errors...
      */

      if (errno != EINTR && errno != EAGAIN)
        return (-1);
    }
    else
    {
     /*
      * Write succeeded, update buffer pointer and total count...
      */

      buffer += count;
      total  += (size_t)count;
    }
  }

  return ((ssize_t)bytes);
}


/*
 * '_prSideChannelDoRequest()' - Send a side-channel command to a backend and wait for a response.
 *
 * This function is normally only called by filters, drivers, or port
 * monitors in order to communicate with the backend used by the current
 * printer.  Programs must be prepared to handle timeout or "not
 * implemented" status codes, which indicate that the backend or device
 * do not support the specified side-channel command.
 *
 * The "datalen" parameter must be initialized to the size of the buffer
 * pointed to by the "data" parameter.  _prSideChannelDoRequest() will
 * update the value to contain the number of data bytes in the buffer.
 *
 * @since CUPS 1.3/macOS 10.5@
 */

pr_sc_status_t			/* O  - Status of command */
_prSideChannelDoRequest(
    pr_sc_command_t command,		/* I  - Command to send */
    char              *data,		/* O  - Response data buffer pointer */
    int               *datalen,		/* IO - Size of data buffer on entry, number of bytes in buffer on return */
    double            timeout)		/* I  - Timeout in seconds */
{
  pr_sc_status_t	status;		/* Status of command */
  pr_sc_command_t	rcommand;	/* Response command */


  if (_prSideChannelWrite(command, _PR_SC_STATUS_NONE, NULL, 0, timeout))
    return (_PR_SC_STATUS_TIMEOUT);

  if (_prSideChannelRead(&rcommand, &status, data, datalen, timeout))
    return (_PR_SC_STATUS_TIMEOUT);

  if (rcommand != command)
    return (_PR_SC_STATUS_BAD_MESSAGE);

  return (status);
}


/*
 * '_prSideChannelRead()' - Read a side-channel message.
 *
 * This function is normally only called by backend programs to read
 * commands from a filter, driver, or port monitor program.  The
 * caller must be prepared to handle incomplete or invalid messages
 * and return the corresponding status codes.
 *
 * The "datalen" parameter must be initialized to the size of the buffer
 * pointed to by the "data" parameter.  _prSideChannelDoRequest() will
 * update the value to contain the number of data bytes in the buffer.
 *
 * @since CUPS 1.3/macOS 10.5@
 */

int					/* O - 0 on success, -1 on error */
_prSideChannelRead(
    pr_sc_command_t *command,		/* O - Command code */
    pr_sc_status_t  *status,		/* O - Status code */
    char              *data,		/* O - Data buffer pointer */
    int               *datalen,		/* IO - Size of data buffer on entry, number of bytes in buffer on return */
    double            timeout)		/* I  - Timeout in seconds */
{
  char		*buffer;		/* Message buffer */
  ssize_t	bytes;			/* Bytes read */
  int		templen;		/* Data length from message */
  int		nfds;			/* Number of file descriptors */
#ifdef HAVE_POLL
  struct pollfd	pfd;			/* Poll structure for poll() */
#else /* select() */
  fd_set	input_set;		/* Input set for select() */
  struct timeval stimeout;		/* Timeout value for select() */
#endif /* HAVE_POLL */


 /*
  * Range check input...
  */

  if (!command || !status)
    return (-1);

 /*
  * See if we have pending data on the side-channel socket...
  */

#ifdef HAVE_POLL
  pfd.fd     = _PR_SC_FD;
  pfd.events = POLLIN;

  while ((nfds = poll(&pfd, 1,
		      timeout < 0.0 ? -1 : (int)(timeout * 1000))) < 0 &&
	 (errno == EINTR || errno == EAGAIN))
    ;

#else /* select() */
  FD_ZERO(&input_set);
  FD_SET(_PR_SC_FD, &input_set);

  stimeout.tv_sec  = (int)timeout;
  stimeout.tv_usec = (int)(timeout * 1000000) % 1000000;

  while ((nfds = select(_PR_SC_FD + 1, &input_set, NULL, NULL,
			timeout < 0.0 ? NULL : &stimeout)) < 0 &&
	 (errno == EINTR || errno == EAGAIN))
    ;

#endif /* HAVE_POLL */

  if (nfds < 1)
  {
    *command = _PR_SC_CMD_NONE;
    *status  = nfds==0 ? _PR_SC_STATUS_TIMEOUT : _PR_SC_STATUS_IO_ERROR;
    return (-1);
  }

 /*
  * Read a side-channel message for the format:
  *
  * Byte(s)  Description
  * -------  -------------------------------------------
  * 0        Command code
  * 1        Status code
  * 2-3      Data length (network byte order)
  * 4-N      Data
  */

  if ((buffer = malloc(_CUPS_SC_MAX_BUFFER)) == NULL)
  {
    *command = _PR_SC_CMD_NONE;
    *status  = _PR_SC_STATUS_TOO_BIG;

    return (-1);
  }

  while ((bytes = read(_PR_SC_FD, buffer, _CUPS_SC_MAX_BUFFER)) < 0)
    if (errno != EINTR && errno != EAGAIN)
    {
      free(buffer);

      *command = _PR_SC_CMD_NONE;
      *status  = _PR_SC_STATUS_IO_ERROR;

      return (-1);
    }

 /*
  * Watch for EOF or too few bytes...
  */

  if (bytes < 4)
  {
    free(buffer);

    *command = _PR_SC_CMD_NONE;
    *status  = _PR_SC_STATUS_BAD_MESSAGE;

    return (-1);
  }

 /*
  * Validate the command code in the message...
  */

  if (buffer[0] < _PR_SC_CMD_SOFT_RESET ||
      buffer[0] >= _PR_SC_CMD_MAX)
  {
    free(buffer);

    *command = _PR_SC_CMD_NONE;
    *status  = _PR_SC_STATUS_BAD_MESSAGE;

    return (-1);
  }

  *command = (pr_sc_command_t)buffer[0];

 /*
  * Validate the data length in the message...
  */

  templen = ((buffer[2] & 255) << 8) | (buffer[3] & 255);

  if (templen > 0 && (!data || !datalen))
  {
   /*
    * Either the response is bigger than the provided buffer or the
    * response is bigger than we've read...
    */

    *status = _PR_SC_STATUS_TOO_BIG;
  }
  else if (!datalen || templen > *datalen || templen > (bytes - 4))
  {
   /*
    * Either the response is bigger than the provided buffer or the
    * response is bigger than we've read...
    */

    *status = _PR_SC_STATUS_TOO_BIG;
  }
  else
  {
   /*
    * The response data will fit, copy it over and provide the actual
    * length...
    */

    *status  = (pr_sc_status_t)buffer[1];
    *datalen = templen;

    memcpy(data, buffer + 4, (size_t)templen);
  }

  free(buffer);

  return (0);
}


/*
 * '_prSideChannelSNMPGet()' - Query a SNMP OID's value.
 *
 * This function asks the backend to do a SNMP OID query on behalf of the
 * filter, port monitor, or backend using the default community name.
 *
 * "oid" contains a numeric OID consisting of integers separated by periods,
 * for example ".1.3.6.1.2.1.43".  Symbolic names from SNMP MIBs are not
 * supported and must be converted to their numeric forms.
 *
 * On input, "data" and "datalen" provide the location and size of the
 * buffer to hold the OID value as a string. HEX-String (binary) values are
 * converted to hexadecimal strings representing the binary data, while
 * NULL-Value and unknown OID types are returned as the empty string.
 * The returned "datalen" does not include the trailing nul.
 *
 * @code _PR_SC_STATUS_NOT_IMPLEMENTED@ is returned by backends that do not
 * support SNMP queries.  @code _PR_SC_STATUS_NO_RESPONSE@ is returned when
 * the printer does not respond to the SNMP query.
 *
 * @since CUPS 1.4/macOS 10.6@
 */

pr_sc_status_t			/* O  - Query status */
_prSideChannelSNMPGet(
    const char *oid,			/* I  - OID to query */
    char       *data,			/* I  - Buffer for OID value */
    int        *datalen,		/* IO - Size of OID buffer on entry, size of value on return */
    double     timeout)			/* I  - Timeout in seconds */
{
  pr_sc_status_t	status;		/* Status of command */
  pr_sc_command_t	rcommand;	/* Response command */
  char			*real_data;	/* Real data buffer for response */
  int			real_datalen,	/* Real length of data buffer */
			real_oidlen;	/* Length of returned OID string */


 /*
  * Range check input...
  */

  if (!oid || !*oid || !data || !datalen || *datalen < 2)
    return (_PR_SC_STATUS_BAD_MESSAGE);

  *data = '\0';

 /*
  * Send the request to the backend and wait for a response...
  */

  if (_prSideChannelWrite(_PR_SC_CMD_SNMP_GET, _PR_SC_STATUS_NONE, oid,
                           (int)strlen(oid) + 1, timeout))
    return (_PR_SC_STATUS_TIMEOUT);

  if ((real_data = malloc(_CUPS_SC_MAX_BUFFER)) == NULL)
    return (_PR_SC_STATUS_TOO_BIG);

  real_datalen = _CUPS_SC_MAX_BUFFER;
  if (_prSideChannelRead(&rcommand, &status, real_data, &real_datalen, timeout))
  {
    free(real_data);
    return (_PR_SC_STATUS_TIMEOUT);
  }

  if (rcommand != _PR_SC_CMD_SNMP_GET)
  {
    free(real_data);
    return (_PR_SC_STATUS_BAD_MESSAGE);
  }

  if (status == _PR_SC_STATUS_OK)
  {
   /*
    * Parse the response of the form "oid\0value"...
    */

    real_oidlen  = (int)strlen(real_data) + 1;
    real_datalen -= real_oidlen;

    if ((real_datalen + 1) > *datalen)
    {
      free(real_data);
      return (_PR_SC_STATUS_TOO_BIG);
    }

    memcpy(data, real_data + real_oidlen, (size_t)real_datalen);
    data[real_datalen] = '\0';

    *datalen = real_datalen;
  }

  free(real_data);

  return (status);
}


/*
 * '_prSideChannelSNMPWalk()' - Query multiple SNMP OID values.
 *
 * This function asks the backend to do multiple SNMP OID queries on behalf
 * of the filter, port monitor, or backend using the default community name.
 * All OIDs under the "parent" OID are queried and the results are sent to
 * the callback function you provide.
 *
 * "oid" contains a numeric OID consisting of integers separated by periods,
 * for example ".1.3.6.1.2.1.43".  Symbolic names from SNMP MIBs are not
 * supported and must be converted to their numeric forms.
 *
 * "timeout" specifies the timeout for each OID query. The total amount of
 * time will depend on the number of OID values found and the time required
 * for each query.
 *
 * "cb" provides a function to call for every value that is found. "context"
 * is an application-defined pointer that is sent to the callback function
 * along with the OID and current data. The data passed to the callback is the
 * same as returned by @link _prSideChannelSNMPGet@.
 *
 * @code _PR_SC_STATUS_NOT_IMPLEMENTED@ is returned by backends that do not
 * support SNMP queries.  @code _PR_SC_STATUS_NO_RESPONSE@ is returned when
 * the printer does not respond to the first SNMP query.
 *
 * @since CUPS 1.4/macOS 10.6@
 */

pr_sc_status_t			/* O - Status of first query of @code _PR_SC_STATUS_OK@ on success */
_prSideChannelSNMPWalk(
    const char          *oid,		/* I - First numeric OID to query */
    double              timeout,	/* I - Timeout for each query in seconds */
    pr_sc_walk_func_t cb,		/* I - Function to call with each value */
    void                *context)	/* I - Application-defined pointer to send to callback */
{
  pr_sc_status_t	status;		/* Status of command */
  pr_sc_command_t	rcommand;	/* Response command */
  char			*real_data;	/* Real data buffer for response */
  int			real_datalen;	/* Real length of data buffer */
  size_t		real_oidlen,	/* Length of returned OID string */
			oidlen;		/* Length of first OID */
  const char		*current_oid;	/* Current OID */
  char			last_oid[2048];	/* Last OID */


 /*
  * Range check input...
  */

  if (!oid || !*oid || !cb)
    return (_PR_SC_STATUS_BAD_MESSAGE);

  if ((real_data = malloc(_CUPS_SC_MAX_BUFFER)) == NULL)
    return (_PR_SC_STATUS_TOO_BIG);

 /*
  * Loop until the OIDs don't match...
  */

  current_oid = oid;
  oidlen      = strlen(oid);
  last_oid[0] = '\0';

  do
  {
   /*
    * Send the request to the backend and wait for a response...
    */

    if (_prSideChannelWrite(_PR_SC_CMD_SNMP_GET_NEXT, _PR_SC_STATUS_NONE,
                             current_oid, (int)strlen(current_oid) + 1, timeout))
    {
      free(real_data);
      return (_PR_SC_STATUS_TIMEOUT);
    }

    real_datalen = _CUPS_SC_MAX_BUFFER;
    if (_prSideChannelRead(&rcommand, &status, real_data, &real_datalen,
                            timeout))
    {
      free(real_data);
      return (_PR_SC_STATUS_TIMEOUT);
    }

    if (rcommand != _PR_SC_CMD_SNMP_GET_NEXT)
    {
      free(real_data);
      return (_PR_SC_STATUS_BAD_MESSAGE);
    }

    if (status == _PR_SC_STATUS_OK)
    {
     /*
      * Parse the response of the form "oid\0value"...
      */

      if (strncmp(real_data, oid, oidlen) || real_data[oidlen] != '.' ||
          !strcmp(real_data, last_oid))
      {
       /*
        * Done with this set of OIDs...
	*/

	free(real_data);
        return (_PR_SC_STATUS_OK);
      }

      if ((size_t)real_datalen < sizeof(real_data))
        real_data[real_datalen] = '\0';

      real_oidlen  = strlen(real_data) + 1;
      real_datalen -= (int)real_oidlen;

     /*
      * Call the callback with the OID and data...
      */

      (*cb)(real_data, real_data + real_oidlen, real_datalen, context);

     /*
      * Update the current OID...
      */

      current_oid = real_data;
      strlcpy(last_oid, current_oid, sizeof(last_oid));
    }
  }
  while (status == _PR_SC_STATUS_OK);

  free(real_data);

  return (status);
}


/*
 * '_prSideChannelWrite()' - Write a side-channel message.
 *
 * This function is normally only called by backend programs to send
 * responses to a filter, driver, or port monitor program.
 *
 * @since CUPS 1.3/macOS 10.5@
 */

int					/* O - 0 on success, -1 on error */
_prSideChannelWrite(
    pr_sc_command_t command,		/* I - Command code */
    pr_sc_status_t  status,		/* I - Status code */
    const char        *data,		/* I - Data buffer pointer */
    int               datalen,		/* I - Number of bytes of data */
    double            timeout)		/* I - Timeout in seconds */
{
  char		*buffer;		/* Message buffer */
  ssize_t	bytes;			/* Bytes written */
#ifdef HAVE_POLL
  struct pollfd	pfd;			/* Poll structure for poll() */
#else /* select() */
  fd_set	output_set;		/* Output set for select() */
  struct timeval stimeout;		/* Timeout value for select() */
#endif /* HAVE_POLL */


 /*
  * Range check input...
  */

  if (command < _PR_SC_CMD_SOFT_RESET || command >= _PR_SC_CMD_MAX ||
      datalen < 0 || datalen > _CUPS_SC_MAX_DATA || (datalen > 0 && !data))
    return (-1);

 /*
  * See if we can safely write to the side-channel socket...
  */

#ifdef HAVE_POLL
  pfd.fd     = _PR_SC_FD;
  pfd.events = POLLOUT;

  if (timeout < 0.0)
  {
    if (poll(&pfd, 1, -1) < 1)
      return (-1);
  }
  else if (poll(&pfd, 1, (int)(timeout * 1000)) < 1)
    return (-1);

#else /* select() */
  FD_ZERO(&output_set);
  FD_SET(_PR_SC_FD, &output_set);

  if (timeout < 0.0)
  {
    if (select(_PR_SC_FD + 1, NULL, &output_set, NULL, NULL) < 1)
      return (-1);
  }
  else
  {
    stimeout.tv_sec  = (int)timeout;
    stimeout.tv_usec = (int)(timeout * 1000000) % 1000000;

    if (select(_PR_SC_FD + 1, NULL, &output_set, NULL, &stimeout) < 1)
      return (-1);
  }
#endif /* HAVE_POLL */

 /*
  * Write a side-channel message in the format:
  *
  * Byte(s)  Description
  * -------  -------------------------------------------
  * 0        Command code
  * 1        Status code
  * 2-3      Data length (network byte order) <= 16384
  * 4-N      Data
  */

  if ((buffer = malloc((size_t)datalen + 4)) == NULL)
    return (-1);

  buffer[0] = (char)command;
  buffer[1] = (char)status;
  buffer[2] = (char)(datalen >> 8);
  buffer[3] = (char)(datalen & 255);

  bytes = 4;

  if (datalen > 0)
  {
    memcpy(buffer + 4, data, (size_t)datalen);
    bytes += datalen;
  }

  while (write(_PR_SC_FD, buffer, (size_t)bytes) < 0)
    if (errno != EINTR && errno != EAGAIN)
    {
      free(buffer);
      return (-1);
    }

  free(buffer);

  return (0);
}
