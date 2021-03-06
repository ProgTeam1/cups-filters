/*
 *   IEEE-1284 Device ID support functions for OpenPrinting CUPS Filters.
 *
 *   Copyright 2007-2011 by Apple Inc.
 *   Copyright 1997-2007 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "COPYING"
 *   which should have been included with this file.
 *
 * Contents:
 *
 *   ieee1284GetDeviceID()           - Get the IEEE-1284 device ID string and
 *                                     corresponding URI.
 *   ieee1284GetMakeModel()          - Get the make and model string from the 
 *                                     device ID.
 *   ieee1284GetValues()             - Get 1284 device ID keys and values.
 *   ieee1284NormalizeMakeAndModel() - Normalize a product/make-and-model
 *                                     string.
 */

/*
 * Include necessary headers.
 */

#include "ieee1284.h"
#include <cups/cups.h>
#include <string.h>
#include <ctype.h>
#define DEBUG_printf(x)
#define DEBUG_puts(x)
#define _cups_isspace(x) isspace((x) & 255)
#define _cups_strcasecmp strcasecmp
#define _cups_strncasecmp strncasecmp


/*
 * 'ieee1284GetDeviceID()' - Get the IEEE-1284 device ID string and
 *                           corresponding URI.
 */

int					/* O - 0 on success, -1 on failure */
ieee1284GetDeviceID(
    int        fd,			/* I - File descriptor */
    char       *device_id,		/* O - 1284 device ID */
    int        device_id_size,		/* I - Size of buffer */
    char       *make_model,		/* O - Make/model */
    int        make_model_size,		/* I - Size of buffer */
    const char *scheme,			/* I - URI scheme */
    char       *uri,			/* O - Device URI */
    int        uri_size)		/* I - Size of buffer */
{
#ifdef __APPLE__ /* This function is a no-op */
  (void)fd;
  (void)device_id;
  (void)device_id_size;
  (void)make_model;
  (void)make_model_size;
  (void)scheme;
  (void)uri;
  (void)uri_size;

  return (-1);

#else /* Get the device ID from the specified file descriptor... */
#  ifdef __linux
  int	length;				/* Length of device ID info */
  int   got_id = 0;
#  endif /* __linux */
#  if defined(__sun) && defined(ECPPIOC_GETDEVID)
  struct ecpp_device_id did;		/* Device ID buffer */
#  endif /* __sun && ECPPIOC_GETDEVID */
  char	*ptr;				/* Pointer into device ID */


  DEBUG_printf(("ieee1284GetDeviceID(fd=%d, device_id=%p, device_id_size=%d, "
                "make_model=%p, make_model_size=%d, scheme=\"%s\", "
		"uri=%p, uri_size=%d)\n", fd, device_id, device_id_size,
		make_model, make_model_size, scheme ? scheme : "(null)",
		uri, uri_size));

 /*
  * Range check input...
  */

  if (!device_id || device_id_size < 32)
  {
    DEBUG_puts("ieee1284GetDeviceID: Bad args!");
    return (-1);
  }

  if (make_model)
    *make_model = '\0';

  if (fd >= 0)
  {
   /*
    * Get the device ID string...
    */

    *device_id = '\0';

#  ifdef __linux
    if (ioctl(fd, LPIOC_GET_DEVICE_ID(device_id_size), device_id))
    {
     /*
      * Linux has to implement things differently for every device it seems.
      * Since the standard parallel port driver does not provide a simple
      * ioctl() to get the 1284 device ID, we have to open the "raw" parallel
      * device corresponding to this port and do some negotiation trickery
      * to get the current device ID.
      */

      if (uri && !strncmp(uri, "parallel:/dev/", 14))
      {
	char	devparport[16];		/* /dev/parportN */
	int	devparportfd,		/* File descriptor for raw device */
		  mode;			/* Port mode */


       /*
	* Since the Linux parallel backend only supports 4 parallel port
	* devices, just grab the trailing digit and use it to construct a
	* /dev/parportN filename...
	*/

	snprintf(devparport, sizeof(devparport), "/dev/parport%s",
		 uri + strlen(uri) - 1);

	if ((devparportfd = open(devparport, O_RDWR | O_NOCTTY)) != -1)
	{
	 /*
	  * Claim the device...
	  */

	  if (!ioctl(devparportfd, PPCLAIM))
	  {
	    fcntl(devparportfd, F_SETFL, fcntl(devparportfd, F_GETFL) | O_NONBLOCK);

	    mode = IEEE1284_MODE_COMPAT;

	    if (!ioctl(devparportfd, PPNEGOT, &mode))
	    {
	     /*
	      * Put the device into Device ID mode...
	      */

	      mode = IEEE1284_MODE_NIBBLE | IEEE1284_DEVICEID;

	      if (!ioctl(devparportfd, PPNEGOT, &mode))
	      {
	       /*
		* Read the 1284 device ID...
		*/

		if ((length = read(devparportfd, device_id,
				   device_id_size - 1)) >= 2)
		{
		  device_id[length] = '\0';
		  got_id = 1;
		}
	      }
	    }

	   /*
	    * Release the device...
	    */

	    ioctl(devparportfd, PPRELEASE);
	  }

	  close(devparportfd);
	}
      }
    }
    else
      got_id = 1;

    if (got_id)
    {
     /*
      * Extract the length of the device ID string from the first two
      * bytes.  The 1284 spec says the length is stored MSB first...
      */

      length = (((unsigned)device_id[0] & 255) << 8) +
	       ((unsigned)device_id[1] & 255);

     /*
      * Check to see if the length is larger than our buffer; first
      * assume that the vendor incorrectly implemented the 1284 spec,
      * and then limit the length to the size of our buffer...
      */

      if (length > device_id_size || length < 14)
	length = (((unsigned)device_id[1] & 255) << 8) +
		 ((unsigned)device_id[0] & 255);

      if (length > device_id_size)
	length = device_id_size;

     /*
      * The length field counts the number of bytes in the string
      * including the length field itself (2 bytes).  The minimum
      * length for a valid/usable device ID is 14 bytes:
      *
      *     <LENGTH> MFG: <MFG> ;MDL: <MDL> ;
      *        2  +   4  +  1  +  5 +  1 +  1
      */

      if (length < 14)
      {
       /*
	* Can't use this device ID, so don't try to copy it...
	*/

	device_id[0] = '\0';
	got_id       = 0;
      }
      else
      {
       /*
	* Copy the device ID text to the beginning of the buffer and
	* nul-terminate.
	*/

	length -= 2;

	memmove(device_id, device_id + 2, length);
	device_id[length] = '\0';
      }
    }
    else
    {
      DEBUG_printf(("ieee1284GetDeviceID: ioctl failed - %s\n",
                    strerror(errno)));
      *device_id = '\0';
    }
#  endif /* __linux */

#   if defined(__sun) && defined(ECPPIOC_GETDEVID)
    did.mode = ECPP_CENTRONICS;
    did.len  = device_id_size - 1;
    did.rlen = 0;
    did.addr = device_id;

    if (!ioctl(fd, ECPPIOC_GETDEVID, &did))
    {
     /*
      * Nul-terminate the device ID text.
      */

      if (did.rlen < (device_id_size - 1))
	device_id[did.rlen] = '\0';
      else
	device_id[device_id_size - 1] = '\0';
    }
#    ifdef DEBUG
    else
      DEBUG_printf(("ieee1284GetDeviceID: ioctl failed - %s\n",
                    strerror(errno)));
#    endif /* DEBUG */
#  endif /* __sun && ECPPIOC_GETDEVID */
  }

 /*
  * Check whether device ID is valid. Turn line breaks and tabs to spaces and
  * reject device IDs with non-printable characters.
  */

  for (ptr = device_id; *ptr; ptr ++)
    if (_cups_isspace(*ptr))
      *ptr = ' ';
    else if ((*ptr & 255) < ' ' || *ptr == 127)
    {
      DEBUG_printf(("ieee1284GetDeviceID: Bad device_id character %d.",
                    *ptr & 255));
      *device_id = '\0';
      break;
    }

  DEBUG_printf(("ieee1284GetDeviceID: device_id=\"%s\"\n", device_id));

  if (scheme && uri)
    *uri = '\0';

  if (!*device_id)
    return (-1);

 /*
  * Get the make and model...
  */

  if (make_model)
    ieee1284GetMakeModel(device_id, make_model, make_model_size);

 /*
  * Then generate a device URI...
  */

  if (scheme && uri && uri_size > 32)
  {
    int			num_values;	/* Number of keys and values */
    cups_option_t	*values;	/* Keys and values in device ID */
    const char		*mfg,		/* Manufacturer */
			*mdl,		/* Model */
			*sern;		/* Serial number */
    char		temp[256],	/* Temporary manufacturer string */
			*tempptr;	/* Pointer into temp string */


   /*
    * Get the make, model, and serial numbers...
    */

    num_values = ieee1284GetValues(device_id, &values);

    if ((sern = cupsGetOption("SERIALNUMBER", num_values, values)) == NULL)
      if ((sern = cupsGetOption("SERN", num_values, values)) == NULL)
        sern = cupsGetOption("SN", num_values, values);

    if ((mfg = cupsGetOption("MANUFACTURER", num_values, values)) == NULL)
      mfg = cupsGetOption("MFG", num_values, values);

    if ((mdl = cupsGetOption("MODEL", num_values, values)) == NULL)
      mdl = cupsGetOption("MDL", num_values, values);

    if (mfg)
    {
      if (!_cups_strcasecmp(mfg, "Hewlett-Packard"))
        mfg = "HP";
      else if (!_cups_strcasecmp(mfg, "Lexmark International"))
        mfg = "Lexmark";
    }
    else
    {
      strncpy(temp, make_model, sizeof(temp) - 1);
      temp[sizeof(temp) - 1] = '\0';

      if ((tempptr = strchr(temp, ' ')) != NULL)
        *tempptr = '\0';

      mfg = temp;
    }

    if (!mdl)
      mdl = "";

    if (!_cups_strncasecmp(mdl, mfg, strlen(mfg)))
    {
      mdl += strlen(mfg);

      while (isspace(*mdl & 255))
        mdl ++;
    }

   /*
    * Generate the device URI from the manufacturer, make_model, and
    * serial number strings.
    */

    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, uri_size, scheme, NULL, mfg, 0,
                     "/%s%s%s", mdl, sern ? "?serial=" : "", sern ? sern : "");

    cupsFreeOptions(num_values, values);
  }

  return (0);
#endif /* __APPLE__ */
}


/*
 * 'ieee1284GetMakeModel()' - Get the make and model string from the device ID.
 */

int					/* O - 0 on success, -1 on failure */
ieee1284GetMakeModel(
    const char *device_id,		/* O - 1284 device ID */
    char       *make_model,		/* O - Make/model */
    int        make_model_size)		/* I - Size of buffer */
{
  int		num_values;		/* Number of keys and values */
  cups_option_t	*values;		/* Keys and values */
  const char	*mfg,			/* Manufacturer string */
		*mdl,			/* Model string */
		*des;			/* Description string */


  DEBUG_printf(("ieee1284GetMakeModel(device_id=\"%s\", "
                "make_model=%p, make_model_size=%d)\n", device_id,
		make_model, make_model_size));

 /*
  * Range check input...
  */

  if (!device_id || !*device_id || !make_model || make_model_size < 32)
  {
    DEBUG_puts("ieee1284GetMakeModel: Bad args!");
    return (-1);
  }

  *make_model = '\0';

 /*
  * Look for the description field...
  */

  num_values = ieee1284GetValues(device_id, &values);

  if ((mdl = cupsGetOption("MODEL", num_values, values)) == NULL)
    mdl = cupsGetOption("MDL", num_values, values);

  if (mdl)
  {
   /*
    * Build a make-model string from the manufacturer and model attributes...
    */

    if ((mfg = cupsGetOption("MANUFACTURER", num_values, values)) == NULL)
      mfg = cupsGetOption("MFG", num_values, values);

    if (!mfg || !_cups_strncasecmp(mdl, mfg, strlen(mfg)))
    {
     /*
      * Just copy the model string, since it has the manufacturer...
      */

      ieee1284NormalizeMakeAndModel(mdl, make_model, make_model_size);
    }
    else
    {
     /*
      * Concatenate the make and model...
      */

      char	temp[1024];		/* Temporary make and model */

      snprintf(temp, sizeof(temp), "%s %s", mfg, mdl);

      ieee1284NormalizeMakeAndModel(temp, make_model, make_model_size);
    }
  }
  else if ((des = cupsGetOption("DESCRIPTION", num_values, values)) != NULL ||
           (des = cupsGetOption("DES", num_values, values)) != NULL)
  {
   /*
    * Make sure the description contains something useful, since some
    * printer manufacturers (HP) apparently don't follow the standards
    * they helped to define...
    *
    * Here we require the description to be 8 or more characters in length,
    * containing at least one space and one letter.
    */

    if (strlen(des) >= 8)
    {
      const char	*ptr;		/* Pointer into description */
      int		letters,	/* Number of letters seen */
			spaces;		/* Number of spaces seen */


      for (ptr = des, letters = 0, spaces = 0; *ptr; ptr ++)
      {
	if (isspace(*ptr & 255))
	  spaces ++;
	else if (isalpha(*ptr & 255))
	  letters ++;

	if (spaces && letters)
	  break;
      }

      if (spaces && letters)
        ieee1284NormalizeMakeAndModel(des, make_model, make_model_size);
    }
  }

  if (!make_model[0])
  {
   /*
    * Use "Unknown" as the printer make and model...
    */

    strncpy(make_model, "Unknown", make_model_size - 1);
    make_model[make_model_size - 1] = '\0';
  }

  cupsFreeOptions(num_values, values);

  return (0);
}


/*
 * 'ieee1284GetValues()' - Get 1284 device ID keys and values.
 *
 * The returned dictionary is a CUPS option array that can be queried with
 * cupsGetOption and freed with cupsFreeOptions.
 */

int					/* O - Number of key/value pairs */
ieee1284GetValues(
    const char *device_id,		/* I - IEEE-1284 device ID string */
    cups_option_t **values)		/* O - Array of key/value pairs */
{
  int		num_values;		/* Number of values */
  char		key[256],		/* Key string */
		value[256],		/* Value string */
		*ptr;			/* Pointer into key/value */


 /*
  * Range check input...
  */

  if (values)
    *values = NULL;

  if (!device_id || !values)
    return (0);

 /*
  * Parse the 1284 device ID value into keys and values.  The format is
  * repeating sequences of:
  *
  *   [whitespace]key:value[whitespace];
  */

  num_values = 0;
  while (*device_id)
  {
    while (_cups_isspace(*device_id))
      device_id ++;

    if (!*device_id)
      break;

    for (ptr = key; *device_id && *device_id != ':'; device_id ++)
      if (ptr < (key + sizeof(key) - 1))
        *ptr++ = *device_id;

    if (!*device_id)
      break;

    while (ptr > key && _cups_isspace(ptr[-1]))
      ptr --;

    *ptr = '\0';
    device_id ++;

    while (_cups_isspace(*device_id))
      device_id ++;

    if (!*device_id)
      break;

    for (ptr = value; *device_id && *device_id != ';'; device_id ++)
      if (ptr < (value + sizeof(value) - 1))
        *ptr++ = *device_id;

    if (!*device_id)
      break;

    while (ptr > value && _cups_isspace(ptr[-1]))
      ptr --;

    *ptr = '\0';
    device_id ++;

    num_values = cupsAddOption(key, value, num_values, values);
  }

  return (num_values);
}


/*
 * 'ieee1284NormalizeMakeAndModel()' - Normalize a product/make-and-model
 *                                     string.
 *
 * This function tries to undo the mistakes made by many printer manufacturers
 * to produce a clean make-and-model string we can use.
 */

char *					/* O - Normalized make-and-model string
                                           or NULL on error */
ieee1284NormalizeMakeAndModel(
    const char *make_and_model,		/* I - Original make-and-model string */
    char       *buffer,			/* I - String buffer */
    size_t     bufsize)			/* I - Size of string buffer */
{
  char	*bufptr;			/* Pointer into buffer */


  if (!make_and_model || !buffer || bufsize < 1)
  {
    if (buffer)
      *buffer = '\0';

    return (NULL);
  }

 /*
  * Skip leading whitespace...
  */

  while (_cups_isspace(*make_and_model))
    make_and_model ++;

 /*
  * Remove parenthesis and add manufacturers as needed...
  */

  if (make_and_model[0] == '(')
  {
    strncpy(buffer, make_and_model + 1, bufsize - 1);
    buffer[bufsize - 1] = '\0';

    if ((bufptr = strrchr(buffer, ')')) != NULL)
      *bufptr = '\0';
  }
  else if (!_cups_strncasecmp(make_and_model, "XPrint", 6))
  {
   /*
    * Xerox XPrint...
    */

    snprintf(buffer, bufsize, "Xerox %s", make_and_model);
  }
  else if (!_cups_strncasecmp(make_and_model, "Eastman", 7))
  {
   /*
    * Kodak...
    */

    snprintf(buffer, bufsize, "Kodak %s", make_and_model + 7);
  }
  else if (!_cups_strncasecmp(make_and_model, "laserwriter", 11))
  {
   /*
    * Apple LaserWriter...
    */

    snprintf(buffer, bufsize, "Apple LaserWriter%s", make_and_model + 11);
  }
  else if (!_cups_strncasecmp(make_and_model, "colorpoint", 10))
  {
   /*
    * Seiko...
    */

    snprintf(buffer, bufsize, "Seiko %s", make_and_model);
  }
  else if (!_cups_strncasecmp(make_and_model, "fiery", 5))
  {
   /*
    * EFI...
    */

    snprintf(buffer, bufsize, "EFI %s", make_and_model);
  }
  else if (!_cups_strncasecmp(make_and_model, "ps ", 3) ||
	   !_cups_strncasecmp(make_and_model, "colorpass", 9))
  {
   /*
    * Canon...
    */

    snprintf(buffer, bufsize, "Canon %s", make_and_model);
  }
  else if (!_cups_strncasecmp(make_and_model, "primera", 7))
  {
   /*
    * Fargo...
    */

    snprintf(buffer, bufsize, "Fargo %s", make_and_model);
  }
  else if (!_cups_strncasecmp(make_and_model, "designjet", 9) ||
           !_cups_strncasecmp(make_and_model, "deskjet", 7))
  {
   /*
    * HP...
    */

    snprintf(buffer, bufsize, "HP %s", make_and_model);
  }
  else
  {
    strncpy(buffer, make_and_model, bufsize - 1);
    buffer[bufsize - 1] = '\0';
  }

 /*
  * Clean up the make...
  */

  if (!_cups_strncasecmp(buffer, "agfa", 4))
  {
   /*
    * Replace with AGFA (all uppercase)...
    */

    buffer[0] = 'A';
    buffer[1] = 'G';
    buffer[2] = 'F';
    buffer[3] = 'A';
  }
  else if (!_cups_strncasecmp(buffer, "Hewlett-Packard hp ", 19))
  {
   /*
    * Just put "HP" on the front...
    */

    buffer[0] = 'H';
    buffer[1] = 'P';
    memmove(buffer + 2, buffer + 18, strlen(buffer + 18) + 1);
  }
  else if (!_cups_strncasecmp(buffer, "Hewlett-Packard ", 16))
  {
   /*
    * Just put "HP" on the front...
    */

    buffer[0] = 'H';
    buffer[1] = 'P';
    memmove(buffer + 2, buffer + 15, strlen(buffer + 15) + 1);
  }
  else if (!_cups_strncasecmp(buffer, "Lexmark International", 21))
  {
   /*
    * Strip "International"...
    */

    memmove(buffer + 8, buffer + 21, strlen(buffer + 21) + 1);
  }
  else if (!_cups_strncasecmp(buffer, "herk", 4))
  {
   /*
    * Replace with LHAG...
    */

    buffer[0] = 'L';
    buffer[1] = 'H';
    buffer[2] = 'A';
    buffer[3] = 'G';
  }
  else if (!_cups_strncasecmp(buffer, "linotype", 8))
  {
   /*
    * Replace with LHAG...
    */

    buffer[0] = 'L';
    buffer[1] = 'H';
    buffer[2] = 'A';
    buffer[3] = 'G';
    memmove(buffer + 4, buffer + 8, strlen(buffer + 8) + 1);
  }

 /*
  * Remove trailing whitespace and return...
  */

  for (bufptr = buffer + strlen(buffer) - 1;
       bufptr >= buffer && _cups_isspace(*bufptr);
       bufptr --);

  bufptr[1] = '\0';

  return (buffer[0] ? buffer : NULL);
}
