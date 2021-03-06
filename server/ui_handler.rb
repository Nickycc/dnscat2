# session_handler.rb
# By Ron Bowes
# Created May, 2014

module UiHandler
  def initialize_ui_handler()
    @pending = {}
    @uis = []
  end

  def add_pending(id)
    @pending[id] = true
  end

  # Callback
  def ui_created(ui, id, force = false)
    if(force || @pending[id])
      @pending.delete(id)
      @uis << ui
      ui.parent = self
    end
  end

  def pending_count()
    return @pending.length
  end

  def each_child_ui()
    @uis.each do |ui|
      yield(ui)
    end
  end

  def display_uis(all, where = self, indent = "")
    where.puts(indent + self.to_s)
    indent += " "
    each_child_ui() do |ui|
      if(all || ui.active? || ui.activity?)
        if(ui.respond_to?(:display_uis))
          ui.display_uis(all, where, indent + " ")
        else
          where.puts(indent + ui.to_s)
        end
      end

      if(all && pending_count() > 0)
        where.puts()
        where.puts("We also have %d pending sessions" % pending_count())
      end
    end
  end
end
