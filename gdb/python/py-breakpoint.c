/* Python interface to breakpoints

   Copyright (C) 2008-2021 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "value.h"
#include "python-internal.h"
#include "python.h"
#include "charset.h"
#include "breakpoint.h"
#include "gdbcmd.h"
#include "gdbthread.h"
#include "observable.h"
#include "cli/cli-script.h"
#include "ada-lang.h"
#include "arch-utils.h"
#include "language.h"
#include "location.h"
#include "py-event.h"
#include "linespec.h"

/* Debugging of Python breakpoints.  */

static bool pybp_debug;

/* Implementation of "show debug py-breakpoint".  */

static void
show_pybp_debug (struct ui_file *file, int from_tty,
		 struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Python breakpoint debugging is %s.\n"), value);
}

/* Print a "py-breakpoint" debug statement.  */

#define pybp_debug_printf(fmt, ...) \
  debug_prefixed_printf_cond (pybp_debug, "py-breakpoint", fmt, ##__VA_ARGS__)

/* Print a "py-breakpoint" enter/exit debug statements.  */

#define PYBP_SCOPED_DEBUG_ENTER_EXIT \
  scoped_debug_enter_exit (pybp_debug, "py-breakpoint")

/* Number of live breakpoints.  */
static int bppy_live;

/* Variables used to pass information between the Breakpoint
   constructor and the breakpoint-created hook function.  */
gdbpy_breakpoint_object *bppy_pending_object;

/* Function that is called when a Python condition is evaluated.  */
static const char stop_func[] = "stop";

/* This is used to initialize various gdb.bp_* constants.  */
struct pybp_code
{
  /* The name.  */
  const char *name;
  /* The code.  */
  int code;
};

/* Entries related to the type of user set breakpoints.  */
static struct pybp_code pybp_codes[] =
{
  { "BP_NONE", bp_none},
  { "BP_BREAKPOINT", bp_breakpoint},
  { "BP_HARDWARE_BREAKPOINT", bp_hardware_breakpoint},
  { "BP_WATCHPOINT", bp_watchpoint},
  { "BP_HARDWARE_WATCHPOINT", bp_hardware_watchpoint},
  { "BP_READ_WATCHPOINT", bp_read_watchpoint},
  { "BP_ACCESS_WATCHPOINT", bp_access_watchpoint},
  { "BP_CATCHPOINT", bp_catchpoint},
  {NULL} /* Sentinel.  */
};

/* Entries related to the type of watchpoint.  */
static struct pybp_code pybp_watch_types[] =
{
  { "WP_READ", hw_read},
  { "WP_WRITE", hw_write},
  { "WP_ACCESS", hw_access},
  {NULL} /* Sentinel.  */
};

/* Python function which checks the validity of a breakpoint object.  */
static PyObject *
bppy_is_valid (PyObject *self, PyObject *args)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  if (self_bp->bp)
    Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

/* Python function to test whether or not the breakpoint is enabled.  */
static PyObject *
bppy_get_enabled (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);
  if (! self_bp->bp)
    Py_RETURN_FALSE;
  if (self_bp->bp->enable_state == bp_enabled)
    Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

/* Python function to test whether or not the breakpoint is silent.  */
static PyObject *
bppy_get_silent (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);
  if (self_bp->bp->silent)
    Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

/* Python function to set the enabled state of a breakpoint.  */
static int
bppy_set_enabled (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  int cmp;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `enabled' attribute."));

      return -1;
    }
  else if (! PyBool_Check (newvalue))
    {
      PyErr_SetString (PyExc_TypeError,
		       _("The value of `enabled' must be a boolean."));
      return -1;
    }

  cmp = PyObject_IsTrue (newvalue);
  if (cmp < 0)
    return -1;

  try
    {
      if (cmp == 1)
	enable_breakpoint (self_bp->bp);
      else
	disable_breakpoint (self_bp->bp);
    }
  catch (const gdb_exception &except)
    {
      GDB_PY_SET_HANDLE_EXCEPTION (except);
    }

  return 0;
}

/* Python function to set the 'silent' state of a breakpoint.  */
static int
bppy_set_silent (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  int cmp;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `silent' attribute."));
      return -1;
    }
  else if (! PyBool_Check (newvalue))
    {
      PyErr_SetString (PyExc_TypeError,
		       _("The value of `silent' must be a boolean."));
      return -1;
    }

  cmp = PyObject_IsTrue (newvalue);
  if (cmp < 0)
    return -1;
  else
    breakpoint_set_silent (self_bp->bp, cmp);

  return 0;
}

/* Python function to set the thread of a breakpoint.  */
static int
bppy_set_thread (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  long id;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `thread' attribute."));
      return -1;
    }
  else if (PyInt_Check (newvalue))
    {
      if (! gdb_py_int_as_long (newvalue, &id))
	return -1;

      if (!valid_global_thread_id (id))
	{
	  PyErr_SetString (PyExc_RuntimeError,
			   _("Invalid thread ID."));
	  return -1;
	}
    }
  else if (newvalue == Py_None)
    id = -1;
  else
    {
      PyErr_SetString (PyExc_TypeError,
		       _("The value of `thread' must be an integer or None."));
      return -1;
    }

  breakpoint_set_thread (self_bp->bp, id);

  return 0;
}

/* Python function to set the (Ada) task of a breakpoint.  */
static int
bppy_set_task (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  long id;
  int valid_id = 0;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `task' attribute."));
      return -1;
    }
  else if (PyInt_Check (newvalue))
    {
      if (! gdb_py_int_as_long (newvalue, &id))
	return -1;

      try
	{
	  valid_id = valid_task_id (id);
	}
      catch (const gdb_exception &except)
	{
	  GDB_PY_SET_HANDLE_EXCEPTION (except);
	}

      if (! valid_id)
	{
	  PyErr_SetString (PyExc_RuntimeError,
			   _("Invalid task ID."));
	  return -1;
	}
    }
  else if (newvalue == Py_None)
    id = 0;
  else
    {
      PyErr_SetString (PyExc_TypeError,
		       _("The value of `task' must be an integer or None."));
      return -1;
    }

  breakpoint_set_task (self_bp->bp, id);

  return 0;
}

/* Python function which deletes the underlying GDB breakpoint.  This
   triggers the breakpoint_deleted observer which will call
   gdbpy_breakpoint_deleted; that function cleans up the Python
   sections.  */

static PyObject *
bppy_delete_breakpoint (PyObject *self, PyObject *args)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  try
    {
      delete_breakpoint (self_bp->bp);
    }
  catch (const gdb_exception &except)
    {
      GDB_PY_HANDLE_EXCEPTION (except);
    }

  Py_RETURN_NONE;
}


/* Python function to set the ignore count of a breakpoint.  */
static int
bppy_set_ignore_count (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  long value;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `ignore_count' attribute."));
      return -1;
    }
  else if (! PyInt_Check (newvalue))
    {
      PyErr_SetString (PyExc_TypeError,
		       _("The value of `ignore_count' must be an integer."));
      return -1;
    }

  if (! gdb_py_int_as_long (newvalue, &value))
    return -1;

  if (value < 0)
    value = 0;

  try
    {
      set_ignore_count (self_bp->number, (int) value, 0);
    }
  catch (const gdb_exception &except)
    {
      GDB_PY_SET_HANDLE_EXCEPTION (except);
    }

  return 0;
}

/* Python function to set the hit count of a breakpoint.  */
static int
bppy_set_hit_count (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `hit_count' attribute."));
      return -1;
    }
  else
    {
      long value;

      if (! gdb_py_int_as_long (newvalue, &value))
	return -1;

      if (value != 0)
	{
	  PyErr_SetString (PyExc_AttributeError,
			   _("The value of `hit_count' must be zero."));
	  return -1;
	}
    }

  self_bp->bp->hit_count = 0;

  return 0;
}

/* Python function to get the location of a breakpoint.  */
static PyObject *
bppy_get_location (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *obj = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (obj);

  if (obj->bp->type != bp_breakpoint
      && obj->bp->type != bp_hardware_breakpoint)
    Py_RETURN_NONE;

  const char *str = event_location_to_string (obj->bp->location.get ());
  if (! str)
    str = "";
  return host_string_to_python_string (str).release ();
}

/* Python function to get the breakpoint expression.  */
static PyObject *
bppy_get_expression (PyObject *self, void *closure)
{
  const char *str;
  gdbpy_breakpoint_object *obj = (gdbpy_breakpoint_object *) self;
  struct watchpoint *wp;

  BPPY_REQUIRE_VALID (obj);

  if (!is_watchpoint (obj->bp))
    Py_RETURN_NONE;

  wp = (struct watchpoint *) obj->bp;

  str = wp->exp_string.get ();
  if (! str)
    str = "";

  return host_string_to_python_string (str).release ();
}

/* Python function to get the condition expression of a breakpoint.  */
static PyObject *
bppy_get_condition (PyObject *self, void *closure)
{
  char *str;
  gdbpy_breakpoint_object *obj = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (obj);

  str = obj->bp->cond_string.get ();
  if (! str)
    Py_RETURN_NONE;

  return host_string_to_python_string (str).release ();
}

/* Returns 0 on success.  Returns -1 on error, with a python exception set.
   */

static int
bppy_set_condition (PyObject *self, PyObject *newvalue, void *closure)
{
  gdb::unique_xmalloc_ptr<char> exp_holder;
  const char *exp = NULL;
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  struct gdb_exception except;

  BPPY_SET_REQUIRE_VALID (self_bp);

  if (newvalue == NULL)
    {
      PyErr_SetString (PyExc_TypeError,
		       _("Cannot delete `condition' attribute."));
      return -1;
    }
  else if (newvalue == Py_None)
    exp = "";
  else
    {
      exp_holder = python_string_to_host_string (newvalue);
      if (exp_holder == NULL)
	return -1;
      exp = exp_holder.get ();
    }

  try
    {
      set_breakpoint_condition (self_bp->bp, exp, 0, false);
    }
  catch (gdb_exception &ex)
    {
      except = std::move (ex);
    }

  GDB_PY_SET_HANDLE_EXCEPTION (except);

  return 0;
}

/* Python function to get the commands attached to a breakpoint.  */
static PyObject *
bppy_get_commands (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  struct breakpoint *bp = self_bp->bp;

  BPPY_REQUIRE_VALID (self_bp);

  if (! self_bp->bp->commands)
    Py_RETURN_NONE;

  string_file stb;

  current_uiout->redirect (&stb);
  try
    {
      print_command_lines (current_uiout, breakpoint_commands (bp), 0);
    }
  catch (const gdb_exception &except)
    {
      current_uiout->redirect (NULL);
      gdbpy_convert_exception (except);
      return NULL;
    }

  current_uiout->redirect (NULL);
  return host_string_to_python_string (stb.c_str ()).release ();
}

/* Set the commands attached to a breakpoint.  Returns 0 on success.
   Returns -1 on error, with a python exception set.  */
static int
bppy_set_commands (PyObject *self, PyObject *newvalue, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;
  struct gdb_exception except;

  BPPY_SET_REQUIRE_VALID (self_bp);

  gdb::unique_xmalloc_ptr<char> commands
    (python_string_to_host_string (newvalue));
  if (commands == nullptr)
    return -1;

  try
    {
      bool first = true;
      char *save_ptr = nullptr;
      auto reader
	= [&] ()
	  {
	    const char *result = strtok_r (first ? commands.get () : nullptr,
					   "\n", &save_ptr);
	    first = false;
	    return result;
	  };

      counted_command_line lines = read_command_lines_1 (reader, 1, nullptr);
      breakpoint_set_commands (self_bp->bp, std::move (lines));
    }
  catch (gdb_exception &ex)
    {
      except = std::move (ex);
    }

  GDB_PY_SET_HANDLE_EXCEPTION (except);

  return 0;
}

/* Python function to get the breakpoint type.  */
static PyObject *
bppy_get_type (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  return gdb_py_object_from_longest (self_bp->bp->type).release ();
}

/* Python function to get the visibility of the breakpoint.  */

static PyObject *
bppy_get_visibility (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  if (user_breakpoint_p (self_bp->bp))
    Py_RETURN_TRUE;

  Py_RETURN_FALSE;
}

/* Python function to determine if the breakpoint is a temporary
   breakpoint.  */

static PyObject *
bppy_get_temporary (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  if (self_bp->bp->disposition == disp_del
      || self_bp->bp->disposition == disp_del_at_next_stop)
    Py_RETURN_TRUE;

  Py_RETURN_FALSE;
}

/* Python function to determine if the breakpoint is a pending
   breakpoint.  */

static PyObject *
bppy_get_pending (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  if (is_watchpoint (self_bp->bp))
    Py_RETURN_FALSE;
  if (pending_breakpoint_p (self_bp->bp))
    Py_RETURN_TRUE;

  Py_RETURN_FALSE;
}

/* Python function to get the breakpoint's number.  */
static PyObject *
bppy_get_number (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  return gdb_py_object_from_longest (self_bp->number).release ();
}

/* Python function to get the breakpoint's thread ID.  */
static PyObject *
bppy_get_thread (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  if (self_bp->bp->thread == -1)
    Py_RETURN_NONE;

  return gdb_py_object_from_longest (self_bp->bp->thread).release ();
}

/* Python function to get the breakpoint's task ID (in Ada).  */
static PyObject *
bppy_get_task (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  if (self_bp->bp->task == 0)
    Py_RETURN_NONE;

  return gdb_py_object_from_longest (self_bp->bp->task).release ();
}

/* Python function to get the breakpoint's hit count.  */
static PyObject *
bppy_get_hit_count (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  return gdb_py_object_from_longest (self_bp->bp->hit_count).release ();
}

/* Python function to get the breakpoint's ignore count.  */
static PyObject *
bppy_get_ignore_count (PyObject *self, void *closure)
{
  gdbpy_breakpoint_object *self_bp = (gdbpy_breakpoint_object *) self;

  BPPY_REQUIRE_VALID (self_bp);

  return gdb_py_object_from_longest (self_bp->bp->ignore_count).release ();
}

/* Internal function to validate the Python parameters/keywords
   provided to bppy_init.  */

static int
bppy_init_validate_args (const char *spec, char *source,
			 char *function, char *label,
			 char *line, enum bptype type)
{
  /* If spec is defined, ensure that none of the explicit location
     keywords are also defined.  */
  if (spec != NULL)
    {
      if (source != NULL || function != NULL || label != NULL || line != NULL)
	{
	  PyErr_SetString (PyExc_RuntimeError,
			   _("Breakpoints specified with spec cannot "
			     "have source, function, label or line defined."));
	  return -1;
	}
    }
  else
    {
      /* If spec isn't defined, ensure that the user is not trying to
	 define a watchpoint with an explicit location.  */
      if (type == bp_watchpoint)
	{
	  PyErr_SetString (PyExc_RuntimeError,
			   _("Watchpoints cannot be set by explicit "
			     "location parameters."));
	  return -1;
	}
      else
	{
	  /* Otherwise, ensure some explicit locations are defined.  */
	  if (source == NULL && function == NULL && label == NULL
	      && line == NULL)
	    {
	      PyErr_SetString (PyExc_RuntimeError,
			       _("Neither spec nor explicit location set."));
	      return -1;
	    }
	  /* Finally, if source is specified, ensure that line, label
	     or function are specified too.  */
	  if (source != NULL && function == NULL && label == NULL
	      && line == NULL)
	    {
	      PyErr_SetString (PyExc_RuntimeError,
			       _("Specifying a source must also include a "
				 "line, label or function."));
	      return -1;
	    }
	}
    }
  return 1;
}

/* Python function to create a new breakpoint.  */
static int
bppy_init (PyObject *self, PyObject *args, PyObject *kwargs)
{
  static const char *keywords[] = { "spec", "type", "wp_class", "internal",
				    "temporary","source", "function",
				    "label", "line", "qualified", NULL };
  const char *spec = NULL;
  enum bptype type = bp_breakpoint;
  int access_type = hw_write;
  PyObject *internal = NULL;
  PyObject *temporary = NULL;
  PyObject *lineobj = NULL;;
  int internal_bp = 0;
  int temporary_bp = 0;
  gdb::unique_xmalloc_ptr<char> line;
  char *label = NULL;
  char *source = NULL;
  char *function = NULL;
  PyObject * qualified = NULL;

  if (!gdb_PyArg_ParseTupleAndKeywords (args, kwargs, "|siiOOsssOO", keywords,
					&spec, &type, &access_type,
					&internal,
					&temporary, &source,
					&function, &label, &lineobj,
					&qualified))
    return -1;


  if (lineobj != NULL)
    {
      if (PyInt_Check (lineobj))
	line.reset (xstrprintf ("%ld", PyInt_AsLong (lineobj)));
      else if (PyString_Check (lineobj))
	line = python_string_to_host_string (lineobj);
      else
	{
	  PyErr_SetString (PyExc_RuntimeError,
			   _("Line keyword should be an integer or a string. "));
	  return -1;
	}
    }

  if (internal)
    {
      internal_bp = PyObject_IsTrue (internal);
      if (internal_bp == -1)
	return -1;
    }

  if (temporary != NULL)
    {
      temporary_bp = PyObject_IsTrue (temporary);
      if (temporary_bp == -1)
	return -1;
    }

  if (bppy_init_validate_args (spec, source, function, label, line.get (),
			       type) == -1)
    return -1;

  bppy_pending_object = (gdbpy_breakpoint_object *) self;
  bppy_pending_object->number = -1;
  bppy_pending_object->bp = NULL;

  try
    {
      switch (type)
	{
	case bp_breakpoint:
	case bp_hardware_breakpoint:
	  {
	    event_location_up location;
	    symbol_name_match_type func_name_match_type
	      = (qualified != NULL && PyObject_IsTrue (qualified)
		  ? symbol_name_match_type::FULL
		  : symbol_name_match_type::WILD);

	    if (spec != NULL)
	      {
		gdb::unique_xmalloc_ptr<char>
		  copy_holder (xstrdup (skip_spaces (spec)));
		const char *copy = copy_holder.get ();

		location  = string_to_event_location (&copy,
						      current_language,
						      func_name_match_type);
	      }
	    else
	      {
		struct explicit_location explicit_loc;

		initialize_explicit_location (&explicit_loc);
		explicit_loc.source_filename = source;
		explicit_loc.function_name = function;
		explicit_loc.label_name = label;

		if (line != NULL)
		  explicit_loc.line_offset =
		    linespec_parse_line_offset (line.get ());

		explicit_loc.func_name_match_type = func_name_match_type;

		location = new_explicit_location (&explicit_loc);
	      }

	    const struct breakpoint_ops *ops =
	      breakpoint_ops_for_event_location (location.get (), false);

	    create_breakpoint (python_gdbarch,
			       location.get (), NULL, -1, NULL, false,
			       0,
			       temporary_bp, type,
			       0,
			       AUTO_BOOLEAN_TRUE,
			       ops,
			       0, 1, internal_bp, 0);
	    break;
	  }
	case bp_watchpoint:
	  {
	    gdb::unique_xmalloc_ptr<char>
	      copy_holder (xstrdup (skip_spaces (spec)));
	    char *copy = copy_holder.get ();

	    if (access_type == hw_write)
	      watch_command_wrapper (copy, 0, internal_bp);
	    else if (access_type == hw_access)
	      awatch_command_wrapper (copy, 0, internal_bp);
	    else if (access_type == hw_read)
	      rwatch_command_wrapper (copy, 0, internal_bp);
	    else
	      error(_("Cannot understand watchpoint access type."));
	    break;
	  }
	case bp_catchpoint:
	  error (_("BP_CATCHPOINT not supported"));
	default:
	  error(_("Do not understand breakpoint type to set."));
	}
    }
  catch (const gdb_exception &except)
    {
      bppy_pending_object = NULL;
      gdbpy_convert_exception (except);
      return -1;
    }

  BPPY_SET_REQUIRE_VALID ((gdbpy_breakpoint_object *) self);
  return 0;
}

/* Append to LIST the breakpoint Python object associated to B.

   Return true on success.  Return false on failure, with the Python error
   indicator set.  */

static bool
build_bp_list (struct breakpoint *b, PyObject *list)
{
  PyObject *bp = (PyObject *) b->py_bp_object;

  /* Not all breakpoints will have a companion Python object.
     Only breakpoints that were created via bppy_new, or
     breakpoints that were created externally and are tracked by
     the Python Scripting API.  */
  if (bp == nullptr)
    return true;

  return PyList_Append (list, bp) == 0;
}

/* Static function to return a tuple holding all breakpoints.  */

PyObject *
gdbpy_breakpoints (PyObject *self, PyObject *args)
{
  if (bppy_live == 0)
    return PyTuple_New (0);

  gdbpy_ref<> list (PyList_New (0));
  if (list == NULL)
    return NULL;

  /* If build_bp_list returns false, it signals an error condition.  In that
     case abandon building the list and return nullptr.  */
  for (breakpoint *bp : all_breakpoints ())
    if (!build_bp_list (bp, list.get ()))
      return nullptr;

  return PyList_AsTuple (list.get ());
}

/* Call the "stop" method (if implemented) in the breakpoint
   class.  If the method returns True, the inferior  will be
   stopped at the breakpoint.  Otherwise the inferior will be
   allowed to continue.  */

enum ext_lang_bp_stop
gdbpy_breakpoint_cond_says_stop (const struct extension_language_defn *extlang,
				 struct breakpoint *b)
{
  int stop;
  struct gdbpy_breakpoint_object *bp_obj = b->py_bp_object;
  PyObject *py_bp = (PyObject *) bp_obj;
  struct gdbarch *garch;

  if (bp_obj == NULL)
    return EXT_LANG_BP_STOP_UNSET;

  stop = -1;
  garch = b->gdbarch ? b->gdbarch : get_current_arch ();

  gdbpy_enter enter_py (garch, current_language);

  if (bp_obj->is_finish_bp)
    bpfinishpy_pre_stop_hook (bp_obj);

  if (PyObject_HasAttrString (py_bp, stop_func))
    {
      gdbpy_ref<> result (PyObject_CallMethod (py_bp, stop_func, NULL));

      stop = 1;
      if (result != NULL)
	{
	  int evaluate = PyObject_IsTrue (result.get ());

	  if (evaluate == -1)
	    gdbpy_print_stack ();

	  /* If the "stop" function returns False that means
	     the Python breakpoint wants GDB to continue.  */
	  if (! evaluate)
	    stop = 0;
	}
      else
	gdbpy_print_stack ();
    }

  if (bp_obj->is_finish_bp)
    bpfinishpy_post_stop_hook (bp_obj);

  if (stop < 0)
    return EXT_LANG_BP_STOP_UNSET;
  return stop ? EXT_LANG_BP_STOP_YES : EXT_LANG_BP_STOP_NO;
}

/* Checks if the  "stop" method exists in this breakpoint.
   Used by condition_command to ensure mutual exclusion of breakpoint
   conditions.  */

int
gdbpy_breakpoint_has_cond (const struct extension_language_defn *extlang,
			   struct breakpoint *b)
{
  PyObject *py_bp;
  struct gdbarch *garch;

  if (b->py_bp_object == NULL)
    return 0;

  py_bp = (PyObject *) b->py_bp_object;
  garch = b->gdbarch ? b->gdbarch : get_current_arch ();

  gdbpy_enter enter_py (garch, current_language);
  return PyObject_HasAttrString (py_bp, stop_func);
}



/* Event callback functions.  */

/* Callback that is used when a breakpoint is created.  This function
   will create a new Python breakpoint object.  */
static void
gdbpy_breakpoint_created (struct breakpoint *bp)
{
  PYBP_SCOPED_DEBUG_ENTER_EXIT;

  gdbpy_breakpoint_object *newbp;

  if (!user_breakpoint_p (bp) && bppy_pending_object == NULL)
    {
      pybp_debug_printf ("not attaching python object to this breakpoint");
      return;
    }

  if (bp->type != bp_breakpoint
      && bp->type != bp_hardware_breakpoint
      && bp->type != bp_watchpoint
      && bp->type != bp_hardware_watchpoint
      && bp->type != bp_read_watchpoint
      && bp->type != bp_access_watchpoint
      && bp->type != bp_catchpoint)
    {
      pybp_debug_printf ("is not a breakpoint or watchpoint");
      return;
    }

  struct gdbarch *garch = bp->gdbarch ? bp->gdbarch : get_current_arch ();
  gdbpy_enter enter_py (garch, current_language);

  if (bppy_pending_object)
    {
      newbp = bppy_pending_object;
      Py_INCREF (newbp);
      bppy_pending_object = NULL;
      pybp_debug_printf ("attaching existing breakpoint object");
    }
  else
    {
      newbp = PyObject_New (gdbpy_breakpoint_object, &breakpoint_object_type);
      pybp_debug_printf ("attaching new breakpoint object");
    }
  if (newbp)
    {
      newbp->number = bp->number;
      newbp->bp = bp;
      newbp->bp->py_bp_object = newbp;
      newbp->is_finish_bp = 0;
      ++bppy_live;
    }
  else
    {
      PyErr_SetString (PyExc_RuntimeError,
		       _("Error while creating breakpoint from GDB."));
      gdbpy_print_stack ();
    }

  if (!evregpy_no_listeners_p (gdb_py_events.breakpoint_created))
    {
      if (evpy_emit_event ((PyObject *) newbp,
			   gdb_py_events.breakpoint_created) < 0)
	gdbpy_print_stack ();
    }
}

/* Callback that is used when a breakpoint is deleted.  This will
   invalidate the corresponding Python object.  */
static void
gdbpy_breakpoint_deleted (struct breakpoint *b)
{
  PYBP_SCOPED_DEBUG_ENTER_EXIT;

  int num = b->number;
  struct breakpoint *bp = NULL;

  bp = get_breakpoint (num);
  if (bp)
    {
      struct gdbarch *garch = bp->gdbarch ? bp->gdbarch : get_current_arch ();
      gdbpy_enter enter_py (garch, current_language);

      gdbpy_ref<gdbpy_breakpoint_object> bp_obj (bp->py_bp_object);
      if (bp_obj != NULL)
	{
	  if (!evregpy_no_listeners_p (gdb_py_events.breakpoint_deleted))
	    {
	      if (evpy_emit_event ((PyObject *) bp_obj.get (),
				   gdb_py_events.breakpoint_deleted) < 0)
		gdbpy_print_stack ();
	    }

	  bp_obj->bp = NULL;
	  --bppy_live;
	}
    }
}

/* Callback that is used when a breakpoint is modified.  */

static void
gdbpy_breakpoint_modified (struct breakpoint *b)
{
  PYBP_SCOPED_DEBUG_ENTER_EXIT;

  int num = b->number;
  struct breakpoint *bp = NULL;

  bp = get_breakpoint (num);
  if (bp)
    {
      struct gdbarch *garch = bp->gdbarch ? bp->gdbarch : get_current_arch ();
      gdbpy_enter enter_py (garch, current_language);

      PyObject *bp_obj = (PyObject *) bp->py_bp_object;
      if (bp_obj)
	{
	  if (!evregpy_no_listeners_p (gdb_py_events.breakpoint_modified))
	    {
	      if (evpy_emit_event (bp_obj,
				   gdb_py_events.breakpoint_modified) < 0)
		gdbpy_print_stack ();
	    }
	}
    }
}



/* Initialize the Python breakpoint code.  */
int
gdbpy_initialize_breakpoints (void)
{
  int i;

  breakpoint_object_type.tp_new = PyType_GenericNew;
  if (PyType_Ready (&breakpoint_object_type) < 0)
    return -1;

  if (gdb_pymodule_addobject (gdb_module, "Breakpoint",
			      (PyObject *) &breakpoint_object_type) < 0)
    return -1;

  gdb::observers::breakpoint_created.attach (gdbpy_breakpoint_created,
					     "py-breakpoint");
  gdb::observers::breakpoint_deleted.attach (gdbpy_breakpoint_deleted,
					     "py-breakpoint");
  gdb::observers::breakpoint_modified.attach (gdbpy_breakpoint_modified,
					      "py-breakpoint");

  /* Add breakpoint types constants.  */
  for (i = 0; pybp_codes[i].name; ++i)
    {
      if (PyModule_AddIntConstant (gdb_module, pybp_codes[i].name,
				   pybp_codes[i].code) < 0)
	return -1;
    }

  /* Add watchpoint types constants.  */
  for (i = 0; pybp_watch_types[i].name; ++i)
    {
      if (PyModule_AddIntConstant (gdb_module, pybp_watch_types[i].name,
				   pybp_watch_types[i].code) < 0)
	return -1;
    }

  return 0;
}



/* Helper function that overrides this Python object's
   PyObject_GenericSetAttr to allow extra validation of the attribute
   being set.  */

static int
local_setattro (PyObject *self, PyObject *name, PyObject *v)
{
  gdbpy_breakpoint_object *obj = (gdbpy_breakpoint_object *) self;
  gdb::unique_xmalloc_ptr<char> attr (python_string_to_host_string (name));

  if (attr == NULL)
    return -1;

  /* If the attribute trying to be set is the "stop" method,
     but we already have a condition set in the CLI or other extension
     language, disallow this operation.  */
  if (strcmp (attr.get (), stop_func) == 0)
    {
      const struct extension_language_defn *extlang = NULL;

      if (obj->bp->cond_string != NULL)
	extlang = get_ext_lang_defn (EXT_LANG_GDB);
      if (extlang == NULL)
	extlang = get_breakpoint_cond_ext_lang (obj->bp, EXT_LANG_PYTHON);
      if (extlang != NULL)
	{
	  std::string error_text
	    = string_printf (_("Only one stop condition allowed.  There is"
			       " currently a %s stop condition defined for"
			       " this breakpoint."),
			     ext_lang_capitalized_name (extlang));
	  PyErr_SetString (PyExc_RuntimeError, error_text.c_str ());
	  return -1;
	}
    }

  return PyObject_GenericSetAttr (self, name, v);
}

static gdb_PyGetSetDef breakpoint_object_getset[] = {
  { "enabled", bppy_get_enabled, bppy_set_enabled,
    "Boolean telling whether the breakpoint is enabled.", NULL },
  { "silent", bppy_get_silent, bppy_set_silent,
    "Boolean telling whether the breakpoint is silent.", NULL },
  { "thread", bppy_get_thread, bppy_set_thread,
    "Thread ID for the breakpoint.\n\
If the value is a thread ID (integer), then this is a thread-specific breakpoint.\n\
If the value is None, then this breakpoint is not thread-specific.\n\
No other type of value can be used.", NULL },
  { "task", bppy_get_task, bppy_set_task,
    "Thread ID for the breakpoint.\n\
If the value is a task ID (integer), then this is an Ada task-specific breakpoint.\n\
If the value is None, then this breakpoint is not task-specific.\n\
No other type of value can be used.", NULL },
  { "ignore_count", bppy_get_ignore_count, bppy_set_ignore_count,
    "Number of times this breakpoint should be automatically continued.",
    NULL },
  { "number", bppy_get_number, NULL,
    "Breakpoint's number assigned by GDB.", NULL },
  { "hit_count", bppy_get_hit_count, bppy_set_hit_count,
    "Number of times the breakpoint has been hit.\n\
Can be set to zero to clear the count. No other value is valid\n\
when setting this property.", NULL },
  { "location", bppy_get_location, NULL,
    "Location of the breakpoint, as specified by the user.", NULL},
  { "expression", bppy_get_expression, NULL,
    "Expression of the breakpoint, as specified by the user.", NULL},
  { "condition", bppy_get_condition, bppy_set_condition,
    "Condition of the breakpoint, as specified by the user,\
or None if no condition set."},
  { "commands", bppy_get_commands, bppy_set_commands,
    "Commands of the breakpoint, as specified by the user."},
  { "type", bppy_get_type, NULL,
    "Type of breakpoint."},
  { "visible", bppy_get_visibility, NULL,
    "Whether the breakpoint is visible to the user."},
  { "temporary", bppy_get_temporary, NULL,
    "Whether this breakpoint is a temporary breakpoint."},
  { "pending", bppy_get_pending, NULL,
    "Whether this breakpoint is a pending breakpoint."},
  { NULL }  /* Sentinel.  */
};

static PyMethodDef breakpoint_object_methods[] =
{
  { "is_valid", bppy_is_valid, METH_NOARGS,
    "Return true if this breakpoint is valid, false if not." },
  { "delete", bppy_delete_breakpoint, METH_NOARGS,
    "Delete the underlying GDB breakpoint." },
  { NULL } /* Sentinel.  */
};

PyTypeObject breakpoint_object_type =
{
  PyVarObject_HEAD_INIT (NULL, 0)
  "gdb.Breakpoint",		  /*tp_name*/
  sizeof (gdbpy_breakpoint_object), /*tp_basicsize*/
  0,				  /*tp_itemsize*/
  0,				  /*tp_dealloc*/
  0,				  /*tp_print*/
  0,				  /*tp_getattr*/
  0,				  /*tp_setattr*/
  0,				  /*tp_compare*/
  0,				  /*tp_repr*/
  0,				  /*tp_as_number*/
  0,				  /*tp_as_sequence*/
  0,				  /*tp_as_mapping*/
  0,				  /*tp_hash */
  0,				  /*tp_call*/
  0,				  /*tp_str*/
  0,				  /*tp_getattro*/
  (setattrofunc)local_setattro,   /*tp_setattro */
  0,				  /*tp_as_buffer*/
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
  "GDB breakpoint object",	  /* tp_doc */
  0,				  /* tp_traverse */
  0,				  /* tp_clear */
  0,				  /* tp_richcompare */
  0,				  /* tp_weaklistoffset */
  0,				  /* tp_iter */
  0,				  /* tp_iternext */
  breakpoint_object_methods,	  /* tp_methods */
  0,				  /* tp_members */
  breakpoint_object_getset,	  /* tp_getset */
  0,				  /* tp_base */
  0,				  /* tp_dict */
  0,				  /* tp_descr_get */
  0,				  /* tp_descr_set */
  0,				  /* tp_dictoffset */
  bppy_init,			  /* tp_init */
  0,				  /* tp_alloc */
};

void _initialize_py_breakpoint ();
void
_initialize_py_breakpoint ()
{
  add_setshow_boolean_cmd
      ("py-breakpoint", class_maintenance, &pybp_debug,
	_("Set Python breakpoint debugging."),
	_("Show Python breakpoint debugging."),
	_("When on, Python breakpoint debugging is enabled."),
	NULL,
	show_pybp_debug,
	&setdebuglist, &showdebuglist);
}
