/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2016 Gabriel Ivascu <ivascu.gabriel59@gmail.com>
 *
 *  This file is part of Epiphany.
 *
 *  Epiphany is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Epiphany is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Epiphany.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <libgd/gd.h>

G_BEGIN_DECLS

#define EPHY_TYPE_NOTIFICATION_CONTAINER (ephy_notification_container_get_type ())

/* FIXME: Replace this boilerplate with G_DECLARE_FINAL_TYPE. This won't prove
 * trivial, since G_DECLARE_FINAL_TYPE requires that an autocleanup function
 * has been declared for the parent type, and libgd doesn't have one yet.
 */
#define EPHY_NOTIFICATION_CONTAINER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
   EPHY_TYPE_NOTIFICATION_CONTAINER, EphyNotificationContainer))

#define EPHY_IS_NOTIFICATION_CONTAINER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
   EPHY_TYPE_NOTIFICATION_CONTAINER))

typedef struct _EphyNotificationContainer      EphyNotificationContainer;
typedef struct _EphyNotificationContainerClass EphyNotificationContainerClass;

GType                      ephy_notification_container_get_type         (void) G_GNUC_CONST;

EphyNotificationContainer *ephy_notification_container_get_default      (void);

void                       ephy_notification_container_add_notification (EphyNotificationContainer *self,
                                                                         GtkWidget                 *notification);

G_END_DECLS
