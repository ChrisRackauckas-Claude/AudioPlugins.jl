using SciMLTesting, Airwindows

# Docs env does not depend on this sublibrary yet, so rendering is unchecked.
run_qa(Airwindows; api_docs_kwargs = (; rendered = false))
